# Native SRT VideoToolbox Ingest Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an iOS-native SRT MPEG-TS ingest path that receives SRT directly through libsrt, demuxes MPEG-TS in app code, decodes H.264/H.265 with Apple VideoToolbox, and removes FFmpeg HEVC from the iOS build while keeping RTMP/RTMPS H.264 on FFmpeg.

**Architecture:** Introduce an `IngestSession` boundary so `StreamWorker` keeps timing, jitter, recording, metadata, and source switching while protocol/container/decode work moves into backends. Keep `FfmpegIngestSession` behavior-equivalent to today's path for RTMP/RTMPS and fallback; add `NativeSrtIngestSession` for iOS SRT with small testable units: MPEG-TS parser, PES reassembler, H.264/H.265 access-unit splitters, VideoToolbox decoder, and optional AudioToolbox AAC.

**Tech Stack:** C++17, Objective-C++ on iOS, Qt 6.10, CMake, QtTest, FFmpeg for fallback/encoding, libsrt, Apple VideoToolbox, CoreMedia, CoreVideo, AudioToolbox.

**Spec:** `docs/superpowers/specs/2026-06-15-native-srt-videotoolbox-ingest-design.md`

---

## Before You Start

- Work in the feature worktree: `/Users/timo.korkalainen/Development/timo/OpenLiveReplay/.worktrees/ios-ffmpeg-mbedtls`.
- Preserve the existing uncommitted iOS FFmpeg build-config work unless the current task explicitly touches it.
- Use small commits after each task. Do not combine refactor, native parser work, VideoToolbox work, and FFmpeg build-config changes into one commit.
- Desktop unit-test configure:
  ```bash
  cmake -S . -B /tmp/olr-native-srt-tests -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos
  ```
- Desktop unit-test run:
  ```bash
  cmake --build /tmp/olr-native-srt-tests --target all -j8
  ctest --test-dir /tmp/olr-native-srt-tests --output-on-failure
  ```
- iOS configure:
  ```bash
  $HOME/Qt/6.10.1/ios/bin/qt-cmake -S . -B /tmp/olr-native-srt-ios -G Xcode -DQT_HOST_PATH=$HOME/Qt/6.10.1/macos -DOLR_ENABLE_STREAMDECK=OFF
  ```
- iOS build without signing:
  ```bash
  cmake --build /tmp/olr-native-srt-ios --config Debug --target OpenLiveReplay -- CODE_SIGNING_ALLOWED=NO
  ```

## File Map

- `recorder_engine/ingest/ingestsession.h`: common decoded-video/audio callback interface, backend options, backend selector.
- `recorder_engine/ingest/ffmpegingestsession.{h,cpp}`: current FFmpeg open/read/decode/audio-resample behavior moved out of `StreamWorker`.
- `recorder_engine/ingest/mpegtsparser.{h,cpp}`: PAT/PMT parsing, TS continuity tracking, PES packet assembly.
- `recorder_engine/ingest/pespacket.h`: plain data type for PES payload, stream kind, PID, PTS/DTS.
- `recorder_engine/ingest/h26xaccessunit.{h,cpp}`: Annex B H.264/H.265 access-unit splitting and parameter-set collection.
- `recorder_engine/ingest/videotoolboxdecoder.h`: platform-neutral interface for a decoder that accepts compressed access units and emits decoded frames.
- `recorder_engine/ingest/videotoolboxdecoder.mm`: iOS/macOS VideoToolbox implementation.
- `recorder_engine/ingest/videotoolboxdecoder_stub.cpp`: non-Apple stub for tests/configure.
- `recorder_engine/ingest/nativesrtingestsession.{h,cpp}`: libsrt socket receive loop and native MPEG-TS/video decode orchestration.
- `recorder_engine/ingest/audiotoolboxaacdecoder.{h,mm}`: AAC-to-48 kHz stereo S16 decoder for native SRT production parity.
- `recorder_engine/streamworker.{h,cpp}`: shrink to owner of queues, recording timeline, encoder tick, source restart, and chosen ingest backend.
- `CMakeLists.txt`: add new source files, iOS frameworks, libsrt include/link use for native SRT.
- `build-scripts/build_ffmpeg_ios_srt.sh`: remove iOS FFmpeg HEVC pieces once native SRT HEVC is functional.
- `tests/unit/tst_mpegtsparser.cpp`: PAT/PMT/PES tests.
- `tests/unit/tst_h26xaccessunit.cpp`: H.264/H.265 Annex B tests.
- `tests/unit/tst_ingestbackendselector.cpp`: backend routing tests.
- `tests/smoke/check_ios_ffmpeg_build_config.sh`: assert iOS FFmpeg HEVC is absent.

---

### Task 1: Add IngestSession Boundary and Backend Selector

Create a backend interface and selector without changing runtime behavior. The selector chooses native SRT only when explicitly enabled; the default still routes all sources to FFmpeg.

**Files:**
- Create: `recorder_engine/ingest/ingestsession.h`
- Create: `recorder_engine/ingest/ingestsession.cpp`
- Create: `tests/unit/tst_ingestbackendselector.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write failing backend selector tests**

Create `tests/unit/tst_ingestbackendselector.cpp`:

```cpp
#include <QtTest>

#include "recorder_engine/ingest/ingestsession.h"

class TestIngestBackendSelector : public QObject {
    Q_OBJECT
private slots:
    void defaultRoutesEverythingToFfmpeg();
    void nativeSrtFlagRoutesOnlySrtToNative();
};

void TestIngestBackendSelector::defaultRoutesEverythingToFfmpeg() {
    IngestBackendOptions opts;
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("srt://127.0.0.1:9000")), opts),
             IngestBackendKind::Ffmpeg);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("rtmp://127.0.0.1/live/a")), opts),
             IngestBackendKind::Ffmpeg);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("rtmps://example.test/live/a")), opts),
             IngestBackendKind::Ffmpeg);
}

void TestIngestBackendSelector::nativeSrtFlagRoutesOnlySrtToNative() {
    IngestBackendOptions opts;
    opts.preferNativeSrt = true;
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("srt://127.0.0.1:9000")), opts),
             IngestBackendKind::NativeSrt);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("rtmp://127.0.0.1/live/a")), opts),
             IngestBackendKind::Ffmpeg);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("rtmps://example.test/live/a")), opts),
             IngestBackendKind::Ffmpeg);
}

QTEST_GUILESS_MAIN(TestIngestBackendSelector)
#include "tst_ingestbackendselector.moc"
```

- [ ] **Step 2: Register the test and run it to verify failure**

In `tests/unit/CMakeLists.txt`, add:

```cmake
qt_add_executable(tst_ingestbackendselector tst_ingestbackendselector.cpp)
target_link_libraries(tst_ingestbackendselector PRIVATE olr_test_engine Qt6::Test)
add_test(NAME ingestbackendselector COMMAND tst_ingestbackendselector)
```

Run:

```bash
cmake -S . -B /tmp/olr-native-srt-tests -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos
cmake --build /tmp/olr-native-srt-tests --target tst_ingestbackendselector -j8
```

Expected: compile failure because `recorder_engine/ingest/ingestsession.h` does not exist.

- [ ] **Step 3: Add the interface header**

Create `recorder_engine/ingest/ingestsession.h`:

```cpp
#ifndef INGESTSESSION_H
#define INGESTSESSION_H

#include <QByteArray>
#include <QObject>
#include <QUrl>
#include <functional>
#include <atomic>

extern "C" {
#include <libavutil/frame.h>
}

enum class IngestBackendKind {
    Ffmpeg,
    NativeSrt,
};

struct IngestBackendOptions {
    bool preferNativeSrt = false;
};

struct DecodedVideoFrame {
    AVFrame* frame = nullptr;
    int64_t sourcePtsMs = 0;
};

struct DecodedAudioChunk {
    int64_t startSample = -1;
    QByteArray pcmS16Stereo;
};

constexpr int kDecodedAudioBytesPerSample = 2 * int(sizeof(int16_t));

struct IngestCallbacks {
    std::function<bool()> shouldStop;
    std::function<int64_t()> recordingClockMs;
    std::function<void(bool)> setConnected;
    std::function<void(const QString&)> logInfo;
    std::function<void(DecodedVideoFrame)> onVideoFrame;
    std::function<void(DecodedAudioChunk)> onAudioChunk;
};

class IngestSession {
public:
    virtual ~IngestSession() = default;
    virtual bool open(const QUrl& url, const IngestCallbacks& callbacks) = 0;
    virtual void run() = 0;
    virtual void requestStop() = 0;
};

IngestBackendKind selectIngestBackend(const QUrl& url, const IngestBackendOptions& options);

#endif // INGESTSESSION_H
```

- [ ] **Step 4: Add selector implementation**

Create `recorder_engine/ingest/ingestsession.cpp`:

```cpp
#include "ingestsession.h"

IngestBackendKind selectIngestBackend(const QUrl& url, const IngestBackendOptions& options) {
    const QString scheme = url.scheme().toLower();
    if (options.preferNativeSrt && scheme == QStringLiteral("srt")) {
        return IngestBackendKind::NativeSrt;
    }
    return IngestBackendKind::Ffmpeg;
}
```

- [ ] **Step 5: Add files to CMake**

In the main `CMakeLists.txt`, add `recorder_engine/ingest/ingestsession.cpp` to the app source list near other `recorder_engine` files.

In `tests/CMakeLists.txt`, add `recorder_engine/ingest/ingestsession.cpp` to `olr_test_engine`.

- [ ] **Step 6: Run selector test**

Run:

```bash
cmake --build /tmp/olr-native-srt-tests --target tst_ingestbackendselector -j8
ctest --test-dir /tmp/olr-native-srt-tests -R ingestbackendselector --output-on-failure
```

Expected: test passes.

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt tests/CMakeLists.txt tests/unit/CMakeLists.txt \
  recorder_engine/ingest/ingestsession.h recorder_engine/ingest/ingestsession.cpp \
  tests/unit/tst_ingestbackendselector.cpp
git commit -m "feat: add ingest backend selection boundary"
```

---

### Task 2: Extract Current FFmpeg Ingest Into FfmpegIngestSession

Move the existing `setupDecoder()` and capture-loop packet/decode code into `FfmpegIngestSession` without changing behavior. This task should be mechanical and verified by existing e2e tests.

**Files:**
- Create: `recorder_engine/ingest/ffmpegingestsession.h`
- Create: `recorder_engine/ingest/ffmpegingestsession.cpp`
- Modify: `recorder_engine/streamworker.h`
- Modify: `recorder_engine/streamworker.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Add a compile-only test that can instantiate the FFmpeg session**

Append to `tests/unit/tst_ingestbackendselector.cpp`:

```cpp
#include "recorder_engine/ingest/ffmpegingestsession.h"
```

Add this private slot declaration:

```cpp
    void canConstructFfmpegSession();
```

Add this test body before `QTEST_GUILESS_MAIN`:

```cpp
void TestIngestBackendSelector::canConstructFfmpegSession() {
    FfmpegIngestSession session(0);
    QVERIFY(!session.isOpen());
}
```

Run:

```bash
cmake --build /tmp/olr-native-srt-tests --target tst_ingestbackendselector -j8
```

Expected: compile failure because `ffmpegingestsession.h` does not exist.

- [ ] **Step 2: Create FfmpegIngestSession header**

Create `recorder_engine/ingest/ffmpegingestsession.h`:

```cpp
#ifndef FFMPEGINGESTSESSION_H
#define FFMPEGINGESTSESSION_H

#include "ingestsession.h"

#include <QUrl>
#include <atomic>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

class FfmpegIngestSession final : public IngestSession {
public:
    explicit FfmpegIngestSession(int sourceIndex);
    ~FfmpegIngestSession() override;

    bool open(const QUrl& url, const IngestCallbacks& callbacks) override;
    void run() override;
    void requestStop() override;

    bool isOpen() const { return m_inCtx != nullptr && m_decCtx != nullptr; }

private:
    int m_sourceIndex = -1;
    bool m_stopRequested = false;
    IngestCallbacks m_callbacks;
    QUrl m_url;
    AVFormatContext* m_inCtx = nullptr;
    AVCodecContext* m_decCtx = nullptr;
    int m_videoStreamIdx = -1;

    bool setupDecoder();
    void closeAsync();
};

#endif // FFMPEGINGESTSESSION_H
```

- [ ] **Step 3: Create minimal implementation**

Create `recorder_engine/ingest/ffmpegingestsession.cpp`:

```cpp
#include "ffmpegingestsession.h"

#include <QDebug>
#include <QThread>
#include <thread>

FfmpegIngestSession::FfmpegIngestSession(int sourceIndex)
    : m_sourceIndex(sourceIndex) {}

FfmpegIngestSession::~FfmpegIngestSession() {
    requestStop();
    closeAsync();
}

bool FfmpegIngestSession::open(const QUrl& url, const IngestCallbacks& callbacks) {
    m_url = url;
    m_callbacks = callbacks;
    return setupDecoder();
}

void FfmpegIngestSession::run() {
    if (m_callbacks.logInfo) {
        m_callbacks.logInfo(QStringLiteral("FFmpeg ingest session opened"));
    }
}

void FfmpegIngestSession::requestStop() {
    m_stopRequested = true;
}

bool FfmpegIngestSession::setupDecoder() {
    return false;
}

void FfmpegIngestSession::closeAsync() {
    AVCodecContext* decToFree = m_decCtx;
    AVFormatContext* fmtToClose = m_inCtx;
    m_decCtx = nullptr;
    m_inCtx = nullptr;
    std::thread([decToFree, fmtToClose]() mutable {
        if (decToFree) avcodec_free_context(&decToFree);
        if (fmtToClose) avformat_close_input(&fmtToClose);
    }).detach();
}
```

- [ ] **Step 4: Add files to CMake and verify compile**

Add `recorder_engine/ingest/ffmpegingestsession.cpp` to the app source list and to `olr_test_engine`.

Run:

```bash
cmake --build /tmp/olr-native-srt-tests --target tst_ingestbackendselector -j8
ctest --test-dir /tmp/olr-native-srt-tests -R ingestbackendselector --output-on-failure
```

Expected: test passes.

- [ ] **Step 5: Move FFmpeg open/decode logic**

Move `StreamWorker::setupDecoder()` body into `FfmpegIngestSession::setupDecoder()`. Replace direct member references with `m_` session members and callback calls:

```cpp
if (m_callbacks.logInfo) {
    m_callbacks.logInfo(QStringLiteral("avformat_open_input failed: %1").arg(QString::fromUtf8(errbuf)));
}
```

Move the FFmpeg video/audio read loop from `StreamWorker::captureLoop()` into `FfmpegIngestSession::run()`. When a scaled video frame is ready, emit it through:

```cpp
if (m_callbacks.onVideoFrame) {
    DecodedVideoFrame out;
    out.frame = qf.frame;
    out.sourcePtsMs = qf.sourcePts;
    m_callbacks.onVideoFrame(out);
}
```

When an audio chunk is ready, emit:

```cpp
if (m_callbacks.onAudioChunk) {
    DecodedAudioChunk chunk;
    chunk.startSample = startSample;
    chunk.pcmS16Stereo = QByteArray(reinterpret_cast<const char*>(outBuffer),
                                    converted * kDecodedAudioBytesPerSample);
    m_callbacks.onAudioChunk(chunk);
}
```

Do not change the timestamp math in this task. Copy it exactly first; any timestamp simplification must be a separate commit after parity tests pass.

- [ ] **Step 6: Shrink StreamWorker captureLoop to create and run the backend**

In `StreamWorker::captureLoop()`, keep the outer reconnect/source-switch loop, but replace FFmpeg-specific setup with:

```cpp
IngestCallbacks callbacks;
callbacks.shouldStop = [this]() {
    return !m_captureRunning.load(std::memory_order_relaxed) ||
           m_restartCapture.loadRelaxed() != 0;
};
callbacks.recordingClockMs = [this]() { return m_sharedClock ? m_sharedClock->elapsedMs() : 0; };
callbacks.setConnected = [this](bool connected) { setConnected(connected); };
callbacks.logInfo = [this](const QString& message) {
    qDebug() << "Source" << m_sourceIndex << message;
};
callbacks.onVideoFrame = [this](DecodedVideoFrame decoded) {
    if (m_suppressEnqueue.load(std::memory_order_relaxed)) {
        av_frame_free(&decoded.frame);
        return;
    }
    QMutexLocker locker(&m_frameMutex);
    m_frameQueue.enqueue({decoded.frame, decoded.sourcePtsMs});
    m_lastFrameEnqueueAtMs.store(m_monotonic.elapsed(), std::memory_order_relaxed);
};
callbacks.onAudioChunk = [this](DecodedAudioChunk chunk) {
    enqueueAudio(chunk.startSample,
                 reinterpret_cast<const uint8_t*>(chunk.pcmS16Stereo.constData()),
                 chunk.pcmS16Stereo.size() / kAudioBytesPerSample);
};
```

Instantiate `FfmpegIngestSession` for now and run it.

- [ ] **Step 7: Run existing recording e2e tests**

Run:

```bash
cmake --build /tmp/olr-native-srt-tests --target all -j8
ctest --test-dir /tmp/olr-native-srt-tests -R "record|sync" --output-on-failure
```

Expected: existing FFmpeg ingest behavior remains passing.

- [ ] **Step 8: Commit**

```bash
git add CMakeLists.txt tests/CMakeLists.txt tests/unit/tst_ingestbackendselector.cpp \
  recorder_engine/streamworker.h recorder_engine/streamworker.cpp \
  recorder_engine/ingest/ffmpegingestsession.h recorder_engine/ingest/ffmpegingestsession.cpp
git commit -m "refactor: move FFmpeg ingest behind session interface"
```

---

### Task 3: Add MPEG-TS PAT/PMT/PES Parser

Build the native MPEG-TS parser as a pure C++ unit with no libsrt or VideoToolbox dependency.

**Files:**
- Create: `recorder_engine/ingest/pespacket.h`
- Create: `recorder_engine/ingest/mpegtsparser.h`
- Create: `recorder_engine/ingest/mpegtsparser.cpp`
- Create: `tests/unit/tst_mpegtsparser.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write failing MPEG-TS parser tests**

Create `tests/unit/tst_mpegtsparser.cpp` with small packet builders:

```cpp
#include <QtTest>

#include "recorder_engine/ingest/mpegtsparser.h"

static QByteArray tsPacket(quint16 pid, bool payloadStart, const QByteArray& payload) {
    QByteArray pkt(188, char(0xff));
    pkt[0] = char(0x47);
    pkt[1] = char(((payloadStart ? 0x40 : 0x00) | ((pid >> 8) & 0x1f)));
    pkt[2] = char(pid & 0xff);
    pkt[3] = char(0x10);
    const int n = qMin(payload.size(), 184);
    memcpy(pkt.data() + 4, payload.constData(), size_t(n));
    return pkt;
}

class TestMpegTsParser : public QObject {
    Q_OBJECT
private slots:
    void rejectsBadSyncByte();
    void parsesPmtVideoKinds();
};

void TestMpegTsParser::rejectsBadSyncByte() {
    MpegTsParser parser;
    QByteArray pkt(188, char(0xff));
    QList<PesPacket> out;
    QVERIFY(!parser.pushTsPacket(pkt, &out));
}

void TestMpegTsParser::parsesPmtVideoKinds() {
    MpegTsParser parser;
    QByteArray pat;
    pat.append(char(0x00)); // pointer field
    pat.append(QByteArray::fromHex("00b00d0001c100000001f00000000000"));
    QByteArray pmt;
    pmt.append(char(0x00)); // pointer field
    pmt.append(QByteArray::fromHex("02b0170001c10000e100f0001be101f00024e102f0000f00"));
    QList<PesPacket> out;
    QVERIFY(parser.pushTsPacket(tsPacket(0x0000, true, pat), &out));
    QCOMPARE(parser.pmtPid(), 0x1000);
    QVERIFY(parser.pushTsPacket(tsPacket(0x1000, true, pmt), &out));
    QCOMPARE(parser.videoPid(), 0x0101);
    QCOMPARE(parser.videoCodec(), NativeVideoCodec::H264);
}

QTEST_GUILESS_MAIN(TestMpegTsParser)
#include "tst_mpegtsparser.moc"
```

Run:

```bash
cmake --build /tmp/olr-native-srt-tests --target tst_mpegtsparser -j8
```

Expected: compile failure because parser files do not exist.

- [ ] **Step 2: Add parser data types**

Create `recorder_engine/ingest/pespacket.h`:

```cpp
#ifndef PESPACKET_H
#define PESPACKET_H

#include <QByteArray>
#include <QtGlobal>

enum class NativeVideoCodec {
    Unknown,
    H264,
    Hevc,
};

enum class NativeElementaryStreamKind {
    Unknown,
    Video,
    AudioAac,
};

struct PesPacket {
    quint16 pid = 0;
    NativeElementaryStreamKind kind = NativeElementaryStreamKind::Unknown;
    NativeVideoCodec videoCodec = NativeVideoCodec::Unknown;
    qint64 pts90k = -1;
    qint64 dts90k = -1;
    QByteArray payload;
};

#endif // PESPACKET_H
```

Create `recorder_engine/ingest/mpegtsparser.h`:

```cpp
#ifndef MPEGTSPARSER_H
#define MPEGTSPARSER_H

#include "pespacket.h"

#include <QHash>
#include <QList>

class MpegTsParser {
public:
    bool pushTsPacket(const QByteArray& packet, QList<PesPacket>* completedPes);

    quint16 pmtPid() const { return m_pmtPid; }
    quint16 videoPid() const { return m_videoPid; }
    NativeVideoCodec videoCodec() const { return m_videoCodec; }
    quint16 audioPid() const { return m_audioPid; }

private:
    struct PesAssembly {
        NativeElementaryStreamKind kind = NativeElementaryStreamKind::Unknown;
        NativeVideoCodec videoCodec = NativeVideoCodec::Unknown;
        QByteArray bytes;
    };

    quint16 m_pmtPid = 0xffff;
    quint16 m_videoPid = 0xffff;
    NativeVideoCodec m_videoCodec = NativeVideoCodec::Unknown;
    quint16 m_audioPid = 0xffff;
    QHash<quint16, PesAssembly> m_pes;

    void parsePat(const QByteArray& payload);
    void parsePmt(const QByteArray& payload);
    void pushPesPayload(quint16 pid, bool payloadStart, const QByteArray& payload,
                        QList<PesPacket>* completedPes);
    bool flushPes(quint16 pid, QList<PesPacket>* completedPes);
};

#endif // MPEGTSPARSER_H
```

- [ ] **Step 3: Implement the parser**

Create `recorder_engine/ingest/mpegtsparser.cpp`:

```cpp
#include "mpegtsparser.h"

static quint16 read16(const uchar* p) {
    return quint16((p[0] << 8) | p[1]);
}

static qint64 readPts90k(const uchar* p) {
    return ((qint64(p[0] >> 1) & 0x07) << 30) |
           (qint64(read16(p + 1) >> 1) << 15) |
           qint64(read16(p + 3) >> 1);
}

bool MpegTsParser::pushTsPacket(const QByteArray& packet, QList<PesPacket>* completedPes) {
    if (packet.size() != 188 || uchar(packet[0]) != 0x47) return false;
    const bool payloadStart = (uchar(packet[1]) & 0x40) != 0;
    const quint16 pid = quint16(((uchar(packet[1]) & 0x1f) << 8) | uchar(packet[2]));
    const uchar afc = (uchar(packet[3]) >> 4) & 0x03;
    int offset = 4;
    if (afc == 0 || afc == 2) return true;
    if (afc == 3) {
        if (offset >= packet.size()) return false;
        offset += 1 + uchar(packet[offset]);
    }
    if (offset > packet.size()) return false;
    const QByteArray payload = packet.mid(offset);
    if (pid == 0x0000) {
        parsePat(payload);
    } else if (pid == m_pmtPid) {
        parsePmt(payload);
    } else if (pid == m_videoPid || pid == m_audioPid) {
        pushPesPayload(pid, payloadStart, payload, completedPes);
    }
    return true;
}

void MpegTsParser::parsePat(const QByteArray& payload) {
    if (payload.size() < 13) return;
    int off = uchar(payload[0]) + 1;
    if (off + 12 > payload.size() || uchar(payload[off]) != 0x00) return;
    const int sectionLength = ((uchar(payload[off + 1]) & 0x0f) << 8) | uchar(payload[off + 2]);
    const int end = qMin(payload.size(), off + 3 + sectionLength - 4);
    off += 8;
    while (off + 4 <= end) {
        const quint16 program = read16(reinterpret_cast<const uchar*>(payload.constData() + off));
        const quint16 pid = quint16(((uchar(payload[off + 2]) & 0x1f) << 8) | uchar(payload[off + 3]));
        if (program != 0) {
            m_pmtPid = pid;
            return;
        }
        off += 4;
    }
}

void MpegTsParser::parsePmt(const QByteArray& payload) {
    if (payload.size() < 17) return;
    int off = uchar(payload[0]) + 1;
    if (off + 12 > payload.size() || uchar(payload[off]) != 0x02) return;
    const int sectionLength = ((uchar(payload[off + 1]) & 0x0f) << 8) | uchar(payload[off + 2]);
    const int programInfoLength = ((uchar(payload[off + 10]) & 0x0f) << 8) | uchar(payload[off + 11]);
    int es = off + 12 + programInfoLength;
    const int end = qMin(payload.size(), off + 3 + sectionLength - 4);
    while (es + 5 <= end) {
        const uchar streamType = uchar(payload[es]);
        const quint16 pid = quint16(((uchar(payload[es + 1]) & 0x1f) << 8) | uchar(payload[es + 2]));
        const int esInfoLength = ((uchar(payload[es + 3]) & 0x0f) << 8) | uchar(payload[es + 4]);
        if (streamType == 0x1b) {
            m_videoPid = pid;
            m_videoCodec = NativeVideoCodec::H264;
        } else if (streamType == 0x24) {
            m_videoPid = pid;
            m_videoCodec = NativeVideoCodec::Hevc;
        } else if (streamType == 0x0f) {
            m_audioPid = pid;
        }
        es += 5 + esInfoLength;
    }
}

void MpegTsParser::pushPesPayload(quint16 pid, bool payloadStart, const QByteArray& payload,
                                  QList<PesPacket>* completedPes) {
    if (payloadStart) flushPes(pid, completedPes);
    PesAssembly& assembly = m_pes[pid];
    assembly.kind = (pid == m_videoPid) ? NativeElementaryStreamKind::Video
                                        : NativeElementaryStreamKind::AudioAac;
    assembly.videoCodec = (pid == m_videoPid) ? m_videoCodec : NativeVideoCodec::Unknown;
    assembly.bytes.append(payload);
}

bool MpegTsParser::flushPes(quint16 pid, QList<PesPacket>* completedPes) {
    if (!completedPes || !m_pes.contains(pid)) return false;
    PesAssembly assembly = m_pes.take(pid);
    if (assembly.bytes.size() < 9) return false;
    const uchar* p = reinterpret_cast<const uchar*>(assembly.bytes.constData());
    if (p[0] != 0x00 || p[1] != 0x00 || p[2] != 0x01) return false;
    const int headerLength = 9 + p[8];
    if (headerLength > assembly.bytes.size()) return false;
    PesPacket pes;
    pes.pid = pid;
    pes.kind = assembly.kind;
    pes.videoCodec = assembly.videoCodec;
    if ((p[7] & 0x80) && p[8] >= 5) pes.pts90k = readPts90k(p + 9);
    if ((p[7] & 0x40) && p[8] >= 10) pes.dts90k = readPts90k(p + 14);
    pes.payload = assembly.bytes.mid(headerLength);
    completedPes->append(pes);
    return true;
}
```

- [ ] **Step 4: Register files and tests**

Add parser `.cpp` files to app and `olr_test_engine`.

In `tests/unit/CMakeLists.txt`, add:

```cmake
qt_add_executable(tst_mpegtsparser tst_mpegtsparser.cpp)
target_link_libraries(tst_mpegtsparser PRIVATE olr_test_engine Qt6::Test)
add_test(NAME mpegtsparser COMMAND tst_mpegtsparser)
```

- [ ] **Step 5: Run parser tests**

Run:

```bash
cmake -S . -B /tmp/olr-native-srt-tests -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos
cmake --build /tmp/olr-native-srt-tests --target tst_mpegtsparser -j8
ctest --test-dir /tmp/olr-native-srt-tests -R mpegtsparser --output-on-failure
```

Expected: parser tests pass.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt tests/CMakeLists.txt tests/unit/CMakeLists.txt \
  recorder_engine/ingest/pespacket.h recorder_engine/ingest/mpegtsparser.h \
  recorder_engine/ingest/mpegtsparser.cpp tests/unit/tst_mpegtsparser.cpp
git commit -m "feat: add narrow MPEG-TS parser for native ingest"
```

---

### Task 4: Add H.264/H.265 Access-Unit Splitter and Parameter Sets

Split Annex B H.264/H.265 PES payloads into access units and collect parameter sets needed by VideoToolbox.

**Files:**
- Create: `recorder_engine/ingest/h26xaccessunit.h`
- Create: `recorder_engine/ingest/h26xaccessunit.cpp`
- Create: `tests/unit/tst_h26xaccessunit.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt`

- [ ] **Step 1: Write failing access-unit tests**

Create `tests/unit/tst_h26xaccessunit.cpp`:

```cpp
#include <QtTest>

#include "recorder_engine/ingest/h26xaccessunit.h"

class TestH26xAccessUnit : public QObject {
    Q_OBJECT
private slots:
    void collectsH264ParameterSets();
    void collectsHevcParameterSets();
};

void TestH26xAccessUnit::collectsH264ParameterSets() {
    H26xAccessUnitSplitter splitter(NativeVideoCodec::H264);
    const QByteArray payload = QByteArray::fromHex(
        "000000016764001facd940780227e5c05a808080a000000168ebe3cb22c"
        "0000000168ee3c8000000001658884");
    QList<CompressedAccessUnit> units = splitter.pushPesPayload(payload, 90000, 90000);
    QVERIFY(splitter.parameterSets().h264Sps.size() == 1);
    QVERIFY(splitter.parameterSets().h264Pps.size() == 1);
    QCOMPARE(units.size(), 1);
    QCOMPARE(units.first().codec, NativeVideoCodec::H264);
}

void TestH26xAccessUnit::collectsHevcParameterSets() {
    H26xAccessUnitSplitter splitter(NativeVideoCodec::Hevc);
    const QByteArray payload = QByteArray::fromHex(
        "0000000140010c010ffff016000000014201010160000000014401c172b46240"
        "000000012601af09");
    QList<CompressedAccessUnit> units = splitter.pushPesPayload(payload, 90000, 90000);
    QVERIFY(splitter.parameterSets().hevcVps.size() == 1);
    QVERIFY(splitter.parameterSets().hevcSps.size() == 1);
    QVERIFY(splitter.parameterSets().hevcPps.size() == 1);
    QCOMPARE(units.size(), 1);
    QCOMPARE(units.first().codec, NativeVideoCodec::Hevc);
}

QTEST_GUILESS_MAIN(TestH26xAccessUnit)
#include "tst_h26xaccessunit.moc"
```

Run:

```bash
cmake --build /tmp/olr-native-srt-tests --target tst_h26xaccessunit -j8
```

Expected: compile failure because splitter files do not exist.

- [ ] **Step 2: Add splitter header**

Create `recorder_engine/ingest/h26xaccessunit.h`:

```cpp
#ifndef H26XACCESSUNIT_H
#define H26XACCESSUNIT_H

#include "pespacket.h"

#include <QList>

struct H26xParameterSets {
    QList<QByteArray> h264Sps;
    QList<QByteArray> h264Pps;
    QList<QByteArray> hevcVps;
    QList<QByteArray> hevcSps;
    QList<QByteArray> hevcPps;
};

struct CompressedAccessUnit {
    NativeVideoCodec codec = NativeVideoCodec::Unknown;
    qint64 pts90k = -1;
    qint64 dts90k = -1;
    QByteArray annexB;
    H26xParameterSets parameterSets;
};

class H26xAccessUnitSplitter {
public:
    explicit H26xAccessUnitSplitter(NativeVideoCodec codec);
    QList<CompressedAccessUnit> pushPesPayload(const QByteArray& payload, qint64 pts90k, qint64 dts90k);
    H26xParameterSets parameterSets() const { return m_parameterSets; }

private:
    NativeVideoCodec m_codec = NativeVideoCodec::Unknown;
    H26xParameterSets m_parameterSets;
    QByteArray m_pending;

    void inspectNal(const QByteArray& nal);
};

#endif // H26XACCESSUNIT_H
```

- [ ] **Step 3: Add minimal splitter implementation**

Create `recorder_engine/ingest/h26xaccessunit.cpp`:

```cpp
#include "h26xaccessunit.h"

static QList<QByteArray> splitAnnexBNals(const QByteArray& bytes) {
    QList<QByteArray> nals;
    QList<int> starts;
    for (int i = 0; i + 3 < bytes.size(); ++i) {
        if (bytes[i] == 0 && bytes[i + 1] == 0 &&
            ((bytes[i + 2] == 1) || (i + 3 < bytes.size() && bytes[i + 2] == 0 && bytes[i + 3] == 1))) {
            starts.append(i);
        }
    }
    for (int i = 0; i < starts.size(); ++i) {
        const int start = starts[i];
        const int prefix = (bytes[start + 2] == 1) ? 3 : 4;
        const int end = (i + 1 < starts.size()) ? starts[i + 1] : bytes.size();
        if (end > start + prefix) nals.append(bytes.mid(start + prefix, end - start - prefix));
    }
    return nals;
}

H26xAccessUnitSplitter::H26xAccessUnitSplitter(NativeVideoCodec codec)
    : m_codec(codec) {}

QList<CompressedAccessUnit> H26xAccessUnitSplitter::pushPesPayload(const QByteArray& payload,
                                                                   qint64 pts90k,
                                                                   qint64 dts90k) {
    const QList<QByteArray> nals = splitAnnexBNals(payload);
    for (const QByteArray& nal : nals) inspectNal(nal);
    CompressedAccessUnit unit;
    unit.codec = m_codec;
    unit.pts90k = pts90k;
    unit.dts90k = dts90k;
    unit.annexB = payload;
    unit.parameterSets = m_parameterSets;
    return {unit};
}

void H26xAccessUnitSplitter::inspectNal(const QByteArray& nal) {
    if (nal.isEmpty()) return;
    if (m_codec == NativeVideoCodec::H264) {
        const int type = uchar(nal[0]) & 0x1f;
        if (type == 7 && m_parameterSets.h264Sps.isEmpty()) m_parameterSets.h264Sps.append(nal);
        if (type == 8 && m_parameterSets.h264Pps.isEmpty()) m_parameterSets.h264Pps.append(nal);
    } else if (m_codec == NativeVideoCodec::Hevc && nal.size() >= 2) {
        const int type = (uchar(nal[0]) >> 1) & 0x3f;
        if (type == 32 && m_parameterSets.hevcVps.isEmpty()) m_parameterSets.hevcVps.append(nal);
        if (type == 33 && m_parameterSets.hevcSps.isEmpty()) m_parameterSets.hevcSps.append(nal);
        if (type == 34 && m_parameterSets.hevcPps.isEmpty()) m_parameterSets.hevcPps.append(nal);
    }
}
```

- [ ] **Step 4: Register and run tests**

Add the splitter `.cpp` to app and `olr_test_engine`.

In `tests/unit/CMakeLists.txt`, add:

```cmake
qt_add_executable(tst_h26xaccessunit tst_h26xaccessunit.cpp)
target_link_libraries(tst_h26xaccessunit PRIVATE olr_test_engine Qt6::Test)
add_test(NAME h26xaccessunit COMMAND tst_h26xaccessunit)
```

Run:

```bash
cmake -S . -B /tmp/olr-native-srt-tests -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos
cmake --build /tmp/olr-native-srt-tests --target tst_h26xaccessunit -j8
ctest --test-dir /tmp/olr-native-srt-tests -R h26xaccessunit --output-on-failure
```

Expected: tests pass.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt tests/CMakeLists.txt tests/unit/CMakeLists.txt \
  recorder_engine/ingest/h26xaccessunit.h recorder_engine/ingest/h26xaccessunit.cpp \
  tests/unit/tst_h26xaccessunit.cpp
git commit -m "feat: add H26x access-unit splitting for native ingest"
```

---

### Task 5: Add VideoToolbox Decoder Wrapper

Add an Apple-only decoder wrapper that accepts `CompressedAccessUnit` and emits `AVFrame*` in `AV_PIX_FMT_YUV420P`. Keep non-Apple builds compiling with a stub.

**Files:**
- Create: `recorder_engine/ingest/videotoolboxdecoder.h`
- Create: `recorder_engine/ingest/videotoolboxdecoder.mm`
- Create: `recorder_engine/ingest/videotoolboxdecoder_stub.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add decoder interface**

Create `recorder_engine/ingest/videotoolboxdecoder.h`:

```cpp
#ifndef VIDEOTOOLBOXDECODER_H
#define VIDEOTOOLBOXDECODER_H

#include "h26xaccessunit.h"

#include <QString>
#include <functional>

extern "C" {
#include <libavutil/frame.h>
}

class VideoToolboxDecoder {
public:
    using FrameCallback = std::function<void(AVFrame*)>;

    VideoToolboxDecoder(int outputWidth, int outputHeight);
    ~VideoToolboxDecoder();

    bool decode(const CompressedAccessUnit& unit, FrameCallback onFrame, QString* error);
    void reset();

private:
    class Impl;
    Impl* m_impl = nullptr;
};

#endif // VIDEOTOOLBOXDECODER_H
```

- [ ] **Step 2: Add non-Apple stub**

Create `recorder_engine/ingest/videotoolboxdecoder_stub.cpp`:

```cpp
#include "videotoolboxdecoder.h"

#ifndef __APPLE__
class VideoToolboxDecoder::Impl {
public:
    Impl(int, int) {}
};

VideoToolboxDecoder::VideoToolboxDecoder(int outputWidth, int outputHeight)
    : m_impl(new Impl(outputWidth, outputHeight)) {}

VideoToolboxDecoder::~VideoToolboxDecoder() {
    delete m_impl;
}

bool VideoToolboxDecoder::decode(const CompressedAccessUnit&, FrameCallback, QString* error) {
    if (error) *error = QStringLiteral("VideoToolbox is unavailable on this platform");
    return false;
}

void VideoToolboxDecoder::reset() {}
#endif
```

- [ ] **Step 3: Add Apple implementation skeleton**

Create `recorder_engine/ingest/videotoolboxdecoder.mm` with:

```objc
#include "videotoolboxdecoder.h"

#ifdef __APPLE__
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

extern "C" {
#include <libavutil/imgutils.h>
}

class VideoToolboxDecoder::Impl {
public:
    Impl(int w, int h) : width(w), height(h) {}
    ~Impl() { reset(); }

    int width = 0;
    int height = 0;
    CMVideoFormatDescriptionRef format = nullptr;
    VTDecompressionSessionRef session = nullptr;

    void reset() {
        if (session) {
            VTDecompressionSessionInvalidate(session);
            CFRelease(session);
            session = nullptr;
        }
        if (format) {
            CFRelease(format);
            format = nullptr;
        }
    }
};

VideoToolboxDecoder::VideoToolboxDecoder(int outputWidth, int outputHeight)
    : m_impl(new Impl(outputWidth, outputHeight)) {}

VideoToolboxDecoder::~VideoToolboxDecoder() {
    delete m_impl;
}

bool VideoToolboxDecoder::decode(const CompressedAccessUnit&, FrameCallback, QString* error) {
    if (error) *error = QStringLiteral("VideoToolbox skeleton decoder did not emit a frame");
    return false;
}

void VideoToolboxDecoder::reset() {
    m_impl->reset();
}
#endif
```

- [ ] **Step 4: Wire platform sources**

In `CMakeLists.txt`, add:

```cmake
if(APPLE)
    target_sources(OpenLiveReplay PRIVATE recorder_engine/ingest/videotoolboxdecoder.mm)
    target_link_libraries(OpenLiveReplay PRIVATE "-framework VideoToolbox" "-framework CoreMedia" "-framework CoreVideo")
else()
    target_sources(OpenLiveReplay PRIVATE recorder_engine/ingest/videotoolboxdecoder_stub.cpp)
endif()
```

Add the same conditional source choice to `olr_test_engine` in `tests/CMakeLists.txt`.

- [ ] **Step 5: Build desktop tests and iOS app**

Run:

```bash
cmake -S . -B /tmp/olr-native-srt-tests -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos
cmake --build /tmp/olr-native-srt-tests --target all -j8
$HOME/Qt/6.10.1/ios/bin/qt-cmake -S . -B /tmp/olr-native-srt-ios -G Xcode -DQT_HOST_PATH=$HOME/Qt/6.10.1/macos -DOLR_ENABLE_STREAMDECK=OFF
cmake --build /tmp/olr-native-srt-ios --config Debug --target OpenLiveReplay -- CODE_SIGNING_ALLOWED=NO
```

Expected: both builds pass; native SRT decode is not enabled yet.

- [ ] **Step 6: Implement VideoToolbox session creation and sample submission**

Replace the Apple skeleton `decode()` with logic that:

1. Creates `CMVideoFormatDescriptionRef` from H.264 SPS/PPS using `CMVideoFormatDescriptionCreateFromH264ParameterSets`, or from HEVC VPS/SPS/PPS using `CMVideoFormatDescriptionCreateFromHEVCParameterSets`.
2. Converts Annex B access unit payload into length-prefixed sample data.
3. Creates `CMBlockBufferRef` and `CMSampleBufferRef`.
4. Calls `VTDecompressionSessionDecodeFrame`.
5. Copies output `CVPixelBufferRef` into a new `AV_PIX_FMT_YUV420P` `AVFrame`.

Use this callback shape:

```objc
static void decompressionOutputCallback(void* decompressionOutputRefCon,
                                        void* sourceFrameRefCon,
                                        OSStatus status,
                                        VTDecodeInfoFlags,
                                        CVImageBufferRef imageBuffer,
                                        CMTime,
                                        CMTime) {
    auto* callback = static_cast<VideoToolboxDecoder::FrameCallback*>(sourceFrameRefCon);
    if (status != noErr || !imageBuffer || !callback) return;
    AVFrame* frame = copyPixelBufferToAvFrame(CVPixelBufferRef(imageBuffer));
    if (frame) (*callback)(frame);
}
```

Use a local `copyPixelBufferToAvFrame()` helper that locks the pixel buffer, allocates an `AVFrame`, and copies NV12 planes into YUV420P.

- [ ] **Step 7: Device-build verify**

Run:

```bash
cmake --build /tmp/olr-native-srt-ios --config Debug --target OpenLiveReplay -- CODE_SIGNING_ALLOWED=NO
```

Expected: build passes and links VideoToolbox/CoreMedia/CoreVideo.

- [ ] **Step 8: Commit**

```bash
git add CMakeLists.txt tests/CMakeLists.txt \
  recorder_engine/ingest/videotoolboxdecoder.h \
  recorder_engine/ingest/videotoolboxdecoder.mm \
  recorder_engine/ingest/videotoolboxdecoder_stub.cpp
git commit -m "feat: add VideoToolbox decoder wrapper"
```

---

### Task 6: Add NativeSrtIngestSession With Video-Only SRT MPEG-TS

Add direct libsrt receive and connect it to MPEG-TS parser, H26x splitter, and VideoToolbox decoder. First milestone is video-only; AAC native audio comes next.

**Files:**
- Create: `recorder_engine/ingest/nativesrtingestsession.h`
- Create: `recorder_engine/ingest/nativesrtingestsession.cpp`
- Modify: `recorder_engine/streamworker.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Add NativeSrtIngestSession header**

Create `recorder_engine/ingest/nativesrtingestsession.h`:

```cpp
#ifndef NATIVESRTINGESTSESSION_H
#define NATIVESRTINGESTSESSION_H

#include "h26xaccessunit.h"
#include "ingestsession.h"
#include "mpegtsparser.h"
#include "videotoolboxdecoder.h"

#include <QUrl>
#include <atomic>
#include <memory>

class NativeSrtIngestSession final : public IngestSession {
public:
    NativeSrtIngestSession(int sourceIndex, int outputWidth, int outputHeight,
                           std::atomic<bool>* captureRunning);
    ~NativeSrtIngestSession() override;

    bool open(const QUrl& url, const IngestCallbacks& callbacks) override;
    void run() override;
    void requestStop() override;

private:
    int m_sourceIndex = -1;
    int m_outputWidth = 1920;
    int m_outputHeight = 1080;
    std::atomic<bool>* m_captureRunning = nullptr;
    bool m_stopRequested = false;
    QUrl m_url;
    IngestCallbacks m_callbacks;
    MpegTsParser m_tsParser;
    NativeVideoCodec m_activeCodec = NativeVideoCodec::Unknown;
    std::unique_ptr<H26xAccessUnitSplitter> m_splitter;
    std::unique_ptr<VideoToolboxDecoder> m_decoder;
    int m_socket = -1;

    bool openSocket(QString* error);
    void closeSocket();
    bool shouldStop() const;
};

#endif // NATIVESRTINGESTSESSION_H
```

- [ ] **Step 2: Add NativeSrtIngestSession implementation**

Create `recorder_engine/ingest/nativesrtingestsession.cpp`:

```cpp
#include "nativesrtingestsession.h"

#include <QDebug>
#include <QHostAddress>

#include <srt/srt.h>

NativeSrtIngestSession::NativeSrtIngestSession(int sourceIndex, int outputWidth, int outputHeight,
                                               std::atomic<bool>* captureRunning)
    : m_sourceIndex(sourceIndex),
      m_outputWidth(outputWidth),
      m_outputHeight(outputHeight),
      m_captureRunning(captureRunning) {}

NativeSrtIngestSession::~NativeSrtIngestSession() {
    requestStop();
    closeSocket();
}

bool NativeSrtIngestSession::open(const QUrl& url, const IngestCallbacks& callbacks) {
    m_url = url;
    m_callbacks = callbacks;
    QString error;
    if (!openSocket(&error)) {
        if (m_callbacks.logInfo) m_callbacks.logInfo(error);
        return false;
    }
    if (m_callbacks.setConnected) m_callbacks.setConnected(true);
    return true;
}

void NativeSrtIngestSession::run() {
    QByteArray buffer(1316, 0);
    while (!shouldStop()) {
        const int n = srt_recv(m_socket, buffer.data(), int(buffer.size()));
        if (n <= 0) break;
        for (int off = 0; off + 188 <= n; off += 188) {
            QList<PesPacket> pesPackets;
            if (!m_tsParser.pushTsPacket(buffer.mid(off, 188), &pesPackets)) continue;
            for (const PesPacket& pes : pesPackets) {
                if (pes.kind != NativeElementaryStreamKind::Video) continue;
                if (!m_splitter || m_activeCodec != pes.videoCodec) {
                    m_activeCodec = pes.videoCodec;
                    m_splitter.reset(new H26xAccessUnitSplitter(pes.videoCodec));
                    m_decoder.reset();
                }
                const QList<CompressedAccessUnit> units =
                    m_splitter->pushPesPayload(pes.payload, pes.pts90k, pes.dts90k);
                if (!m_decoder) m_decoder.reset(new VideoToolboxDecoder(m_outputWidth, m_outputHeight));
                for (const CompressedAccessUnit& unit : units) {
                    QString error;
                    const qint64 sourcePtsMs = (unit.pts90k >= 0) ? unit.pts90k / 90 : 0;
                    m_decoder->decode(unit, [this, sourcePtsMs](AVFrame* frame) {
                        if (!m_callbacks.onVideoFrame) {
                            av_frame_free(&frame);
                            return;
                        }
                        DecodedVideoFrame decoded;
                        decoded.frame = frame;
                        decoded.sourcePtsMs = sourcePtsMs;
                        m_callbacks.onVideoFrame(decoded);
                    }, &error);
                    if (!error.isEmpty() && m_callbacks.logInfo) m_callbacks.logInfo(error);
                }
            }
        }
    }
    if (m_callbacks.setConnected) m_callbacks.setConnected(false);
}

void NativeSrtIngestSession::requestStop() {
    m_stopRequested = true;
    closeSocket();
}

bool NativeSrtIngestSession::openSocket(QString* error) {
    srt_startup();
    m_socket = srt_create_socket();
    if (m_socket == SRT_INVALID_SOCK) {
        if (error) *error = QStringLiteral("srt_create_socket failed");
        return false;
    }
    const int no = 0;
    srt_setsockopt(m_socket, 0, SRTO_RCVSYN, &no, sizeof(no));
    const int latency = 500;
    srt_setsockopt(m_socket, 0, SRTO_LATENCY, &latency, sizeof(latency));
    const QByteArray host = m_url.host().toUtf8();
    sockaddr_in sa {};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(quint16(m_url.port(9000)));
    if (inet_pton(AF_INET, host.constData(), &sa.sin_addr) != 1) {
        if (error) *error = QStringLiteral("Native SRT currently requires an IPv4 host");
        closeSocket();
        return false;
    }
    if (srt_connect(m_socket, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == SRT_ERROR) {
        if (error) *error = QStringLiteral("srt_connect failed: %1").arg(QString::fromUtf8(srt_getlasterror_str()));
        closeSocket();
        return false;
    }
    return true;
}

void NativeSrtIngestSession::closeSocket() {
    if (m_socket != -1) {
        srt_close(m_socket);
        m_socket = -1;
    }
}

bool NativeSrtIngestSession::shouldStop() const {
    if (m_stopRequested) return true;
    if (m_captureRunning && !m_captureRunning->load(std::memory_order_relaxed)) return true;
    return m_callbacks.shouldStop ? m_callbacks.shouldStop() : false;
}
```

- [ ] **Step 3: Wire CMake link dependencies**

Add `recorder_engine/ingest/nativesrtingestsession.cpp` to app and test engine.

For iOS, link the existing imported `ffmpeg_srt` target to `OpenLiveReplay` before `NativeSrtIngestSession` is enabled. For desktop tests, keep compiling only if Homebrew SRT headers/libs are available, or exclude the native SRT source from `olr_test_engine` on non-iOS until a direct SRT test fixture exists.

- [ ] **Step 4: Route SRT to native backend behind a flag**

In `StreamWorker::captureLoop()`, create `IngestBackendOptions` and use `selectIngestBackend()`. For this task, set:

```cpp
IngestBackendOptions backendOptions;
#if defined(Q_OS_IOS)
backendOptions.preferNativeSrt = qEnvironmentVariableIsSet("OLR_NATIVE_SRT");
#endif
```

Instantiate:

```cpp
std::unique_ptr<IngestSession> session;
if (selectIngestBackend(QUrl(currentUrl), backendOptions) == IngestBackendKind::NativeSrt) {
    session.reset(new NativeSrtIngestSession(m_sourceIndex, m_targetWidth, m_targetHeight,
                                             &m_captureRunning));
} else {
    session.reset(new FfmpegIngestSession(m_sourceIndex));
}
```

Keep `m_restartCapture` owned by `StreamWorker`; backend sessions stop by calling their own `requestStop()` and by observing `callbacks.shouldStop`.

- [ ] **Step 5: Build iOS with native path compiled**

Run:

```bash
$HOME/Qt/6.10.1/ios/bin/qt-cmake -S . -B /tmp/olr-native-srt-ios -G Xcode -DQT_HOST_PATH=$HOME/Qt/6.10.1/macos -DOLR_ENABLE_STREAMDECK=OFF
cmake --build /tmp/olr-native-srt-ios --config Debug --target OpenLiveReplay -- CODE_SIGNING_ALLOWED=NO
```

Expected: app builds; native SRT is available behind `OLR_NATIVE_SRT`.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt tests/CMakeLists.txt recorder_engine/streamworker.h \
  recorder_engine/streamworker.cpp recorder_engine/ingest/nativesrtingestsession.h \
  recorder_engine/ingest/nativesrtingestsession.cpp
git commit -m "feat: add direct native SRT video ingest path"
```

---

### Task 7: Add AudioToolbox AAC for Native SRT

Add native AAC decode so SRT does not use FFmpeg for audio before decoded frames reach the recorder path.

**Files:**
- Create: `recorder_engine/ingest/audiotoolboxaacdecoder.h`
- Create: `recorder_engine/ingest/audiotoolboxaacdecoder.mm`
- Modify: `recorder_engine/ingest/nativesrtingestsession.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add AAC decoder interface**

Create `recorder_engine/ingest/audiotoolboxaacdecoder.h`:

```cpp
#ifndef AUDIOTOOLBOXAACDECODER_H
#define AUDIOTOOLBOXAACDECODER_H

#include "ingestsession.h"
#include "pespacket.h"

#include <QByteArray>
#include <QString>
#include <functional>

class AudioToolboxAacDecoder {
public:
    using PcmCallback = std::function<void(const DecodedAudioChunk&)>;

    AudioToolboxAacDecoder();
    ~AudioToolboxAacDecoder();

    bool decode(const PesPacket& pes, PcmCallback onPcm, QString* error);
    void reset();

private:
    class Impl;
    Impl* m_impl = nullptr;
};

#endif // AUDIOTOOLBOXAACDECODER_H
```

- [ ] **Step 2: Implement AAC decoder with AudioConverter**

Create `recorder_engine/ingest/audiotoolboxaacdecoder.mm` with an `AudioConverterRef` from AAC input to 48 kHz stereo signed 16-bit interleaved output. The `decode()` method should:

```objc
DecodedAudioChunk chunk;
chunk.startSample = (pes.pts90k >= 0) ? (pes.pts90k / 90) * 48 : -1;
chunk.pcmS16Stereo = QByteArray(reinterpret_cast<const char*>(outputBuffer.data()),
                                int(outputByteCount));
onPcm(chunk);
```

Use `AudioConverterFillComplexBuffer`, rebuild the converter if the AAC stream description changes, and return `false` with a concrete `error` string if AudioToolbox rejects the payload.

- [ ] **Step 3: Wire native audio packets**

In `NativeSrtIngestSession::run()`, handle:

```cpp
if (pes.kind == NativeElementaryStreamKind::AudioAac) {
    QString error;
    m_aacDecoder->decode(pes, [this](const DecodedAudioChunk& chunk) {
        if (m_callbacks.onAudioChunk) m_callbacks.onAudioChunk(chunk);
    }, &error);
    if (!error.isEmpty() && m_callbacks.logInfo) m_callbacks.logInfo(error);
    continue;
}
```

Add `std::unique_ptr<AudioToolboxAacDecoder> m_aacDecoder;` to `NativeSrtIngestSession`.

- [ ] **Step 4: Build iOS**

Run:

```bash
cmake --build /tmp/olr-native-srt-ios --config Debug --target OpenLiveReplay -- CODE_SIGNING_ALLOWED=NO
```

Expected: app builds and links `AudioToolbox.framework`.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt recorder_engine/ingest/audiotoolboxaacdecoder.h \
  recorder_engine/ingest/audiotoolboxaacdecoder.mm \
  recorder_engine/ingest/nativesrtingestsession.h recorder_engine/ingest/nativesrtingestsession.cpp
git commit -m "feat: decode native SRT AAC with AudioToolbox"
```

---

### Task 8: Remove FFmpeg HEVC From iOS Build

Once native SRT HEVC decodes on device, remove FFmpeg HEVC from the iOS build and update smoke tests.

**Files:**
- Modify: `build-scripts/build_ffmpeg_ios_srt.sh`
- Modify: `tests/smoke/check_ios_ffmpeg_build_config.sh`
- Modify: `tests/README.md`

- [ ] **Step 1: Add failing smoke assertions**

In `tests/smoke/check_ios_ffmpeg_build_config.sh`, add rejects:

```sh
reject_in_file "$SCRIPT" '--enable-decoder=hevc'
reject_in_file "$SCRIPT" '--enable-parser=hevc'
reject_in_file "$SCRIPT" '--enable-hwaccel=hevc_videotoolbox'
reject_in_file "$SCRIPT" '--enable-encoder=hevc_videotoolbox'
```

Run:

```bash
bash tests/smoke/check_ios_ffmpeg_build_config.sh
```

Expected: fails while HEVC is still enabled.

- [ ] **Step 2: Remove HEVC from FFmpeg config**

In `build-scripts/build_ffmpeg_ios_srt.sh`, remove:

```sh
--enable-parser=hevc
--enable-decoder=hevc
--enable-hwaccel=hevc_videotoolbox
--enable-encoder=hevc_videotoolbox
```

Keep H.264:

```sh
--enable-parser=h264
--enable-decoder=h264
--enable-hwaccel=h264_videotoolbox
--enable-encoder=h264_videotoolbox
```

- [ ] **Step 3: Update docs**

In `tests/README.md`, update the iOS FFmpeg note to say the iOS FFmpeg build intentionally excludes HEVC because SRT HEVC ingest uses the native VideoToolbox path.

- [ ] **Step 4: Run smoke and iOS dependency build**

Run:

```bash
bash tests/smoke/check_ios_ffmpeg_build_config.sh
bash -n build-scripts/build_ffmpeg_ios_srt.sh
cmake --build /tmp/olr-native-srt-ios --config Debug --target BuildFFmpegDependencies
cmake --build /tmp/olr-native-srt-ios --config Debug --target OpenLiveReplay -- CODE_SIGNING_ALLOWED=NO
```

Expected: smoke passes, dependency build passes, app build passes.

- [ ] **Step 5: Symbol check**

Run:

```bash
nm -gU ios_build/xcframeworks/libavcodec.xcframework/ios-arm64/libavcodec.a | rg 'ff_hevc|hevc_parser|hevc_videotoolbox' || true
```

Expected: no HEVC decoder/parser/hwaccel symbols are printed.

- [ ] **Step 6: Commit**

```bash
git add build-scripts/build_ffmpeg_ios_srt.sh tests/smoke/check_ios_ffmpeg_build_config.sh tests/README.md
git commit -m "build: remove FFmpeg HEVC from iOS build"
```

---

### Task 9: Enable Native SRT By Default on iOS

Make iOS `srt://` use native SRT by default after device tests pass, while leaving RTMP/RTMPS on FFmpeg.

**Files:**
- Modify: `recorder_engine/streamworker.cpp`
- Modify: `tests/unit/tst_ingestbackendselector.cpp`
- Modify: `docs/superpowers/specs/2026-06-15-native-srt-videotoolbox-ingest-design.md`

- [ ] **Step 1: Update selector test for iOS default**

Add a test that documents release behavior:

```cpp
void TestIngestBackendSelector::iosReleasePrefersNativeSrt() {
    IngestBackendOptions opts;
    opts.preferNativeSrt = true;
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("srt://192.0.2.1:9000")), opts),
             IngestBackendKind::NativeSrt);
    QCOMPARE(selectIngestBackend(QUrl(QStringLiteral("rtmps://example.test/live")), opts),
             IngestBackendKind::Ffmpeg);
}
```

Declare the slot in the test class.

- [ ] **Step 2: Make iOS prefer native SRT**

In `StreamWorker::captureLoop()`, set:

```cpp
IngestBackendOptions backendOptions;
#if defined(Q_OS_IOS)
backendOptions.preferNativeSrt = !qEnvironmentVariableIsSet("OLR_FORCE_FFMPEG_SRT");
#endif
```

- [ ] **Step 3: Update spec with final rollout state**

Append to the spec's rollout section:

```markdown
Native SRT is now the iOS default. `OLR_FORCE_FFMPEG_SRT=1` is retained only as a developer fallback while native SRT soaks on real devices.
```

- [ ] **Step 4: Run tests and iOS build**

Run:

```bash
cmake --build /tmp/olr-native-srt-tests --target tst_ingestbackendselector -j8
ctest --test-dir /tmp/olr-native-srt-tests -R ingestbackendselector --output-on-failure
cmake --build /tmp/olr-native-srt-ios --config Debug --target OpenLiveReplay -- CODE_SIGNING_ALLOWED=NO
```

Expected: selector test and iOS build pass.

- [ ] **Step 5: Commit**

```bash
git add recorder_engine/streamworker.cpp tests/unit/tst_ingestbackendselector.cpp \
  docs/superpowers/specs/2026-06-15-native-srt-videotoolbox-ingest-design.md
git commit -m "feat: prefer native SRT ingest on iOS"
```

---

### Task 10: Final Verification

Run the complete verification suite before opening a PR or merging.

**Files:**
- No source changes unless verification finds a defect.

- [ ] **Step 1: Run desktop tests**

```bash
cmake -S . -B /tmp/olr-native-srt-tests -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos
cmake --build /tmp/olr-native-srt-tests --target all -j8
ctest --test-dir /tmp/olr-native-srt-tests --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 2: Run iOS build**

```bash
$HOME/Qt/6.10.1/ios/bin/qt-cmake -S . -B /tmp/olr-native-srt-ios -G Xcode -DQT_HOST_PATH=$HOME/Qt/6.10.1/macos -DOLR_ENABLE_STREAMDECK=OFF
cmake --build /tmp/olr-native-srt-ios --config Debug --target OpenLiveReplay -- CODE_SIGNING_ALLOWED=NO
```

Expected: app builds.

- [ ] **Step 3: Run FFmpeg build-config smoke**

```bash
bash tests/smoke/check_ios_ffmpeg_build_config.sh
git diff --check
```

Expected: both pass.

- [ ] **Step 4: Verify no FFmpeg HEVC symbols**

```bash
nm -gU ios_build/xcframeworks/libavcodec.xcframework/ios-arm64/libavcodec.a | rg 'ff_hevc|hevc_parser|hevc_videotoolbox' || true
```

Expected: no output.

- [ ] **Step 5: Device manual test checklist**

Run on a real iOS device with app signing configured:

```text
1. SRT MPEG-TS H.264 feed connects and records.
2. SRT MPEG-TS HEVC feed connects and records.
3. RTMP H.264 feed connects and records through FFmpeg.
4. RTMPS H.264 feed connects and records through FFmpeg.
5. Source clear paints blue.
6. Source change reconnects.
7. 30-minute SRT HEVC soak records without A/V drift or stalls.
```

- [ ] **Step 6: Commit verification-only fixes**

If verification required fixes, commit them:

```bash
git add <changed files>
git commit -m "fix: stabilize native SRT ingest verification"
```

If no fixes were needed, do not create an empty commit.
