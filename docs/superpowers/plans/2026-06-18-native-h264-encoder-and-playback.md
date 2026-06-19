# Native H.264 Encoder + Hardware Playback Decode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the H.264 codec path functional end-to-end — hardware-only encode (VideoToolbox on mac/iOS, MediaFoundation on Windows) into the existing MKV, and hardware-only H.264 playback decode — so a user who selects H.264 (Plan 1 plumbing) actually records and plays a hardware-coded, intra-only stream.

**Architecture:** A new `NativeVideoEncoder` abstraction with three backends (VideoToolbox `.mm`, MediaFoundation `.cpp`, stub) mirrors the existing `NativeVideoDecoder`. `StreamWorker` and `ReplayManager`'s blue encoder branch to it for H.264; the record path is reordered so a one-frame **priming encode** yields the `avcC` parameter sets that are set on the muxer streams **before** the header is written. Playback routes H.264 file tracks through the existing `NativeVideoDecoder` instead of FFmpeg's software decoder. MPEG-2 stays exactly as today (FFmpeg software).

**Tech Stack:** C++17, Qt 6 (Core/Test), FFmpeg (libav*), Apple VideoToolbox/CoreMedia/CoreVideo, Windows Media Foundation, CMake + Ninja.

## Global Constraints

- **H.264 is hardware-only, encode AND decode. No software H.264 path ever** (licensing: the OS frameworks hold the codec license). Never call FFmpeg's `libx264`/native h264 encoder or software h264 decoder. If a hardware encoder/decoder cannot open, the operation fails cleanly (H.264 unavailable) — never silently fall back to software.
- **MPEG-2 stays software-only** (FFmpeg `mpeg2video`), unchanged.
- **All-intra, both codecs** (every frame a keyframe): VideoToolbox `kVTCompressionPropertyKey_MaxKeyFrameInterval = 1` + `AllowFrameReordering = false` + force-keyframe per frame; MediaFoundation `CODECAPI_AVEncMPVGOPSize = 1`. PTS == DTS (no reordering).
- **H.264 is 4:2:0 8-bit, High profile** (hardware-friendly, not strict broadcast AVC-Intra).
- **Pixel interchange is CPU `AVFrame` YUV420P** (the existing pipeline contract; the encoder consumes it, the decoder produces it).
- **avcC byte format** (mirror `nativevideodecoder_videotoolbox.mm` parameter-set assembly, in reverse): `[0x01, profile, compat, level, 0xFF, 0xE0|nSPS, (uint16 len + SPS)…, nPPS, (uint16 len + PPS)…]`.
- **Default codec is `VideoCodecChoice::Mpeg2Software`** (from Plan 1); H.264 is opt-in and only when hardware is available.
- Build (run from the worktree root): configure once with `cmake -S . -B build/claude-debug -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON`; build `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug <target>`; test `ctest --test-dir build/claude-debug -R <name> -V`. Use `build/claude-debug` only.
- **Platform-gated tests:** native-encoder behavioral tests run only where the hardware exists (macOS for VideoToolbox; Windows CI for MediaFoundation). Where absent, the test asserts `create()` returns null *gracefully*.

---

## File Structure

- **Create** `recorder_engine/codec/nativevideoencoder.h` — abstract `NativeVideoEncoder` + `NativeVideoEncodeCapabilities` + `queryNativeVideoEncodeCapabilities()`.
- **Create** `recorder_engine/codec/nativevideoencoder_stub.cpp` — `create()` → null; capability all-false. Compiled on platforms without a HW backend.
- **Create** `recorder_engine/codec/nativevideoencoder_videotoolbox.mm` — VTCompressionSession backend (macOS + iOS).
- **Create** `recorder_engine/codec/nativevideoencoder_mediafoundation.cpp` — H.264 encoder MFT backend (Windows).
- **Create** `recorder_engine/codec/avcc.h` / `avcc.cpp` — shared `buildAvcCFromParameterSets(sps, pps)` helper (so both backends and tests share one implementation).
- **Modify** `CMakeLists.txt` — compile the encoder backend per platform into the app; add an `olr_test_nativevideoencoder` test lib mirroring `olr_test_nativevideodecoder`.
- **Modify** `recorder_engine/streamworker.h/.cpp` — own a `NativeVideoEncoder` for the H.264 branch; swap the encode block.
- **Modify** `recorder_engine/replaymanager.h/.cpp` — blue encoder via `NativeVideoEncoder` for H.264; priming-encode → avcC → reordered muxer init.
- **Modify** `recorder_engine/muxer.cpp` — harden the H.264 extradata allocation-failure path (Plan 1 carry-forward).
- **Modify** `playback/playbackworker.cpp` (+ `.h` if needed) — route H.264 decoder-bank tracks through `NativeVideoDecoder`.
- **Create** tests: `tests/unit/tst_nativevideoencoder.cpp`, `tests/unit/tst_avcc.cpp`, `tests/unit/tst_h264_roundtrip.cpp`; extend the headless E2E harness for an H.264 record check.

---

## Task 1: avcC builder (shared, pure, fully testable)

**Files:**
- Create: `recorder_engine/codec/avcc.h`, `recorder_engine/codec/avcc.cpp`
- Test: `tests/unit/tst_avcc.cpp`
- Modify: `tests/unit/CMakeLists.txt`, `CMakeLists.txt` (add avcc.cpp to the engine library)

**Interfaces:**
- Produces: `QByteArray buildAvcCFromParameterSets(const QList<QByteArray>& sps, const QList<QByteArray>& pps);` — returns the `avcC` (AVCDecoderConfigurationRecord) bytes, or an empty `QByteArray` if `sps` or `pps` is empty or the first SPS is shorter than 4 bytes.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_avcc.cpp`:

```cpp
// Unit tests for buildAvcCFromParameterSets — the AVCDecoderConfigurationRecord
// (avcC) byte layout used as Matroska CodecPrivate for H.264.
#include <QtTest>

#include "recorder_engine/codec/avcc.h"

class TestAvcc : public QObject {
    Q_OBJECT
private slots:
    void emptyInputsYieldEmpty();
    void buildsWellFormedRecord();
};

void TestAvcc::emptyInputsYieldEmpty() {
    QByteArray sps(QByteArrayLiteral("\x67\x42\x00\x1f"));
    QByteArray pps(QByteArrayLiteral("\x68\xce\x3c\x80"));
    QVERIFY(buildAvcCFromParameterSets({}, {pps}).isEmpty());
    QVERIFY(buildAvcCFromParameterSets({sps}, {}).isEmpty());
    QVERIFY(buildAvcCFromParameterSets({QByteArrayLiteral("\x67\x42")}, {pps}).isEmpty()); // SPS < 4 bytes
}

void TestAvcc::buildsWellFormedRecord() {
    // Minimal SPS/PPS NAL payloads (profile 0x42 baseline, compat 0x00, level 0x1f).
    const QByteArray sps = QByteArrayLiteral("\x67\x42\x00\x1f\x8c\x8d\x40");
    const QByteArray pps = QByteArrayLiteral("\x68\xce\x3c\x80");
    const QByteArray avcc = buildAvcCFromParameterSets({sps}, {pps});

    QVERIFY(!avcc.isEmpty());
    QCOMPARE(quint8(avcc[0]), quint8(0x01));         // configurationVersion
    QCOMPARE(quint8(avcc[1]), quint8(0x42));         // AVCProfileIndication = SPS[1]
    QCOMPARE(quint8(avcc[2]), quint8(0x00));         // profile_compatibility = SPS[2]
    QCOMPARE(quint8(avcc[3]), quint8(0x1f));         // AVCLevelIndication = SPS[3]
    QCOMPARE(quint8(avcc[4]), quint8(0xff));         // 6 reserved bits | lengthSizeMinusOne (3)
    QCOMPARE(quint8(avcc[5]), quint8(0xe1));         // 3 reserved bits | numOfSPS (1)
    const int spsLen = (quint8(avcc[6]) << 8) | quint8(avcc[7]);
    QCOMPARE(spsLen, sps.size());
    QCOMPARE(avcc.mid(8, sps.size()), sps);
    const int ppsCountIdx = 8 + sps.size();
    QCOMPARE(quint8(avcc[ppsCountIdx]), quint8(0x01)); // numOfPPS
    const int ppsLen = (quint8(avcc[ppsCountIdx + 1]) << 8) | quint8(avcc[ppsCountIdx + 2]);
    QCOMPARE(ppsLen, pps.size());
    QCOMPARE(avcc.mid(ppsCountIdx + 3, pps.size()), pps);
}

QTEST_GUILESS_MAIN(TestAvcc)
#include "tst_avcc.moc"
```

Register in `tests/unit/CMakeLists.txt` after the `tst_videocodecchoice` block:

```cmake
olr_add_unit_test(tst_avcc olr_test_engine)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug tst_avcc`
Expected: FAIL to compile — `recorder_engine/codec/avcc.h` not found.

- [ ] **Step 3: Write minimal implementation**

Create `recorder_engine/codec/avcc.h`:

```cpp
#ifndef OLR_AVCC_H
#define OLR_AVCC_H

#include <QByteArray>
#include <QList>

// Build an AVCDecoderConfigurationRecord ("avcC") from H.264 SPS/PPS NAL
// payloads (no start codes / length prefixes — raw NAL bytes). Used as the
// Matroska CodecPrivate for a hardware-encoded H.264 track. Returns an empty
// QByteArray when sps/pps is empty or the first SPS is shorter than 4 bytes
// (profile/compat/level are read from SPS[1..3]).
QByteArray buildAvcCFromParameterSets(const QList<QByteArray>& sps,
                                      const QList<QByteArray>& pps);

#endif // OLR_AVCC_H
```

Create `recorder_engine/codec/avcc.cpp`:

```cpp
#include "recorder_engine/codec/avcc.h"

namespace {
void appendSizedNal(QByteArray* out, const QByteArray& nal) {
    const int size = nal.size();
    out->append(char((size >> 8) & 0xff));
    out->append(char(size & 0xff));
    out->append(nal);
}
} // namespace

QByteArray buildAvcCFromParameterSets(const QList<QByteArray>& sps,
                                      const QList<QByteArray>& pps) {
    if (sps.isEmpty() || pps.isEmpty()) return {};
    const QByteArray& firstSps = sps.first();
    if (firstSps.size() < 4) return {};

    QByteArray avcc;
    avcc.append(char(0x01));               // configurationVersion
    avcc.append(firstSps.at(1));           // AVCProfileIndication
    avcc.append(firstSps.at(2));           // profile_compatibility
    avcc.append(firstSps.at(3));           // AVCLevelIndication
    avcc.append(char(0xff));               // 111111 + lengthSizeMinusOne(3)
    avcc.append(char(0xe0 | (sps.size() & 0x1f))); // 111 + numOfSequenceParameterSets
    for (const QByteArray& s : sps) appendSizedNal(&avcc, s);
    avcc.append(char(pps.size() & 0xff));  // numOfPictureParameterSets
    for (const QByteArray& p : pps) appendSizedNal(&avcc, p);
    return avcc;
}
```

Add `avcc.cpp`/`avcc.h` to the engine library source list in `CMakeLists.txt` (the `olr_engine`/`olr_test_engine` sources near the `recorder_engine/ingest/h26xaccessunit.cpp` entries — add alongside, e.g. after the `recorder_engine/ingest/h26xaccessunit.h ...cpp` line):

```cmake
        recorder_engine/codec/avcc.h recorder_engine/codec/avcc.cpp
```

- [ ] **Step 4: Run test to verify it passes**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug tst_avcc && ctest --test-dir build/claude-debug -R tst_avcc -V`
Expected: PASS (2 tests).

- [ ] **Step 5: Commit**

```bash
git add recorder_engine/codec/avcc.h recorder_engine/codec/avcc.cpp tests/unit/tst_avcc.cpp tests/unit/CMakeLists.txt CMakeLists.txt
git commit -m "feat: add avcC builder for H.264 CodecPrivate"
```

---

## Task 2: NativeVideoEncoder interface + stub + capability probe

**Files:**
- Create: `recorder_engine/codec/nativevideoencoder.h`, `recorder_engine/codec/nativevideoencoder_stub.cpp`
- Test: `tests/unit/tst_nativevideoencoder.cpp`
- Modify: `tests/unit/CMakeLists.txt`, `CMakeLists.txt`

**Interfaces:**
- Produces:
  - `struct NativeVideoEncodeCapabilities { bool h264 = false; QString detail; };`
  - `NativeVideoEncodeCapabilities queryNativeVideoEncodeCapabilities();`
  - ```cpp
    class NativeVideoEncoder {
    public:
      struct Config { int width, height, fpsNum, fpsDen, bitrate; };
      using PacketCallback = std::function<void(const QByteArray& data, int64_t ptsTicks, bool keyframe)>;
      static std::unique_ptr<NativeVideoEncoder> create(const Config&, QString* error);
      virtual ~NativeVideoEncoder();
      virtual bool encode(const AVFrame* frame, int64_t ptsTicks,
                          const PacketCallback& onPacket, QString* error) = 0;
      virtual bool flush(const PacketCallback& onPacket, QString* error) = 0;
      virtual QByteArray avccExtradata() const = 0;   // valid after >=1 successful encode
    protected:
      NativeVideoEncoder() = default;
    };
    ```
- Consumes: `buildAvcCFromParameterSets` (Task 1).

> The `PacketCallback` deals in raw encoded bytes + pts + keyframe, NOT `AVPacket`, so the interface header has no FFmpeg dependency beyond the forward-declared `AVFrame`. The StreamWorker wraps the bytes into an `AVPacket` (Task 6). `ptsTicks` is in the caller's chosen timebase (StreamWorker passes frame counts; the encoder treats it as opaque and echoes it back).

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_nativevideoencoder.cpp`:

```cpp
// Unit tests for NativeVideoEncoder. Behavioral encode tests are gated on the
// platform actually having a hardware H.264 encoder; where absent, create()
// must return nullptr gracefully (never a software fallback).
#include <QtTest>

#include "recorder_engine/codec/nativevideoencoder.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
}

class TestNativeVideoEncoder : public QObject {
    Q_OBJECT
private slots:
    void capabilityProbeIsConsistentWithCreate();
    void encodesIntraFramesWhenAvailable();

private:
    static AVFrame* makeGreyFrame(int w, int h);
};

AVFrame* TestNativeVideoEncoder::makeGreyFrame(int w, int h) {
    AVFrame* f = av_frame_alloc();
    f->format = AV_PIX_FMT_YUV420P;
    f->width = w;
    f->height = h;
    av_frame_get_buffer(f, 32);
    memset(f->data[0], 128, f->linesize[0] * h);
    memset(f->data[1], 128, f->linesize[1] * (h / 2));
    memset(f->data[2], 128, f->linesize[2] * (h / 2));
    return f;
}

void TestNativeVideoEncoder::capabilityProbeIsConsistentWithCreate() {
    const NativeVideoEncodeCapabilities caps = queryNativeVideoEncodeCapabilities();
    QString err;
    auto enc = NativeVideoEncoder::create({1280, 720, 30, 1, 8'000'000}, &err);
    if (caps.h264) {
        QVERIFY2(enc != nullptr, qPrintable("caps say h264 but create failed: " + err));
    } else {
        QVERIFY2(enc == nullptr, "caps say no h264 but create returned an encoder");
    }
}

void TestNativeVideoEncoder::encodesIntraFramesWhenAvailable() {
    QString err;
    auto enc = NativeVideoEncoder::create({1280, 720, 30, 1, 8'000'000}, &err);
    if (!enc) QSKIP("no hardware H.264 encoder on this platform");

    int packets = 0;
    bool allKeyframes = true;
    for (int i = 0; i < 5; ++i) {
        AVFrame* f = makeGreyFrame(1280, 720);
        const bool ok = enc->encode(f, i, [&](const QByteArray& data, int64_t, bool key) {
            ++packets;
            if (!key) allKeyframes = false;
            QVERIFY(!data.isEmpty());
        }, &err);
        av_frame_free(&f);
        QVERIFY2(ok, qPrintable(err));
    }
    enc->flush([&](const QByteArray&, int64_t, bool key) {
        ++packets;
        if (!key) allKeyframes = false;
    }, &err);

    QVERIFY2(packets >= 5, "expected at least one packet per submitted frame");
    QVERIFY2(allKeyframes, "all-intra: every packet must be a keyframe");
    QVERIFY2(!enc->avccExtradata().isEmpty(), "avcC must be available after encoding");
    QCOMPARE(quint8(enc->avccExtradata().at(0)), quint8(0x01)); // configurationVersion
}

QTEST_GUILESS_MAIN(TestNativeVideoEncoder)
#include "tst_nativevideoencoder.moc"
```

- [ ] **Step 2: Register the test with the right per-platform backend lib in `tests/unit/CMakeLists.txt`**

Add after the `tst_avcc` line, mirroring the existing `olr_test_nativevideodecoder` pattern:

```cmake
olr_add_unit_test(tst_nativevideoencoder olr_test_engine)
if(APPLE)
    add_library(olr_test_nativevideoencoder STATIC
        "${CMAKE_SOURCE_DIR}/recorder_engine/codec/nativevideoencoder_videotoolbox.mm")
    target_include_directories(olr_test_nativevideoencoder PRIVATE "${CMAKE_SOURCE_DIR}" "${OLR_FFMPEG_INCLUDE}")
    target_link_directories(olr_test_nativevideoencoder PUBLIC "${OLR_FFMPEG_LIBDIR}")
    target_link_libraries(olr_test_nativevideoencoder
        PUBLIC Qt6::Core avutil olr_test_engine
               "-framework VideoToolbox" "-framework CoreMedia"
               "-framework CoreFoundation" "-framework CoreVideo")
    target_link_libraries(tst_nativevideoencoder PRIVATE olr_test_nativevideoencoder)
elseif(WIN32)
    add_library(olr_test_nativevideoencoder STATIC
        "${CMAKE_SOURCE_DIR}/recorder_engine/codec/nativevideoencoder_mediafoundation.cpp")
    target_include_directories(olr_test_nativevideoencoder PRIVATE "${CMAKE_SOURCE_DIR}" "${OLR_FFMPEG_INCLUDE}")
    target_link_libraries(olr_test_nativevideoencoder
        PUBLIC Qt6::Core olr_test_engine
        PRIVATE mfplat mf mfuuid mfreadwrite strmiids ole32)
    target_link_libraries(tst_nativevideoencoder PRIVATE olr_test_nativevideoencoder)
elseif(NOT WIN32)
    add_library(olr_test_nativevideoencoder STATIC
        "${CMAKE_SOURCE_DIR}/recorder_engine/codec/nativevideoencoder_stub.cpp")
    target_include_directories(olr_test_nativevideoencoder PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(olr_test_nativevideoencoder PUBLIC Qt6::Core olr_test_engine)
    target_link_libraries(tst_nativevideoencoder PRIVATE olr_test_nativevideoencoder)
endif()
```

- [ ] **Step 3: Run test to verify it fails**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug tst_nativevideoencoder`
Expected: FAIL to compile — `nativevideoencoder.h` not found.

- [ ] **Step 4: Write the interface header + stub**

Create `recorder_engine/codec/nativevideoencoder.h` (exactly the Interfaces block above, with includes):

```cpp
#ifndef NATIVEVIDEOENCODER_H
#define NATIVEVIDEOENCODER_H

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>

extern "C" {
struct AVFrame;
}

struct NativeVideoEncodeCapabilities {
    bool h264 = false;
    QString detail;
};

class NativeVideoEncoder {
public:
    struct Config {
        int width = 0;
        int height = 0;
        int fpsNum = 30;
        int fpsDen = 1;
        int bitrate = 30'000'000;
    };
    using PacketCallback =
        std::function<void(const QByteArray& data, int64_t ptsTicks, bool keyframe)>;

    // Returns nullptr (and sets *error) if a hardware H.264 encoder cannot be
    // opened. Never returns a software encoder.
    static std::unique_ptr<NativeVideoEncoder> create(const Config& config, QString* error);

    virtual ~NativeVideoEncoder();

    NativeVideoEncoder(const NativeVideoEncoder&) = delete;
    NativeVideoEncoder& operator=(const NativeVideoEncoder&) = delete;

    // Encode one CPU YUV420P frame (all-intra → one keyframe packet),
    // synchronously draining output to onPacket. ptsTicks is opaque (echoed).
    virtual bool encode(const AVFrame* frame, int64_t ptsTicks,
                        const PacketCallback& onPacket, QString* error) = 0;
    virtual bool flush(const PacketCallback& onPacket, QString* error) = 0;
    virtual QByteArray avccExtradata() const = 0;

protected:
    NativeVideoEncoder() = default;
};

NativeVideoEncodeCapabilities queryNativeVideoEncodeCapabilities();

#endif // NATIVEVIDEOENCODER_H
```

Create `recorder_engine/codec/nativevideoencoder_stub.cpp`:

```cpp
#include "recorder_engine/codec/nativevideoencoder.h"

NativeVideoEncoder::~NativeVideoEncoder() = default;

std::unique_ptr<NativeVideoEncoder> NativeVideoEncoder::create(const Config&, QString* error) {
    if (error) *error = QStringLiteral("No hardware H.264 encoder on this platform");
    return nullptr;
}

NativeVideoEncodeCapabilities queryNativeVideoEncodeCapabilities() {
    NativeVideoEncodeCapabilities caps;
    caps.h264 = false;
    caps.detail = QStringLiteral("No native H.264 encoder backend for this platform");
    return caps;
}
```

Add the encoder backend to the **app** build in `CMakeLists.txt`, mirroring the decoder block (the `if(APPLE) … elseif(WIN32) … elseif(NOT WIN32)` at lines ~269-306): add `recorder_engine/codec/nativevideoencoder_videotoolbox.mm` to the APPLE `target_sources`, `recorder_engine/codec/nativevideoencoder_mediafoundation.cpp` to the WIN32 one, and `recorder_engine/codec/nativevideoencoder_stub.cpp` to the `elseif(NOT WIN32)` one. The APPLE/WIN32 framework links (VideoToolbox / mf*) are already present for the decoder; add `mfreadwrite strmiids` to the Windows link libraries if not already linked.

- [ ] **Step 5: Run test to verify it builds and behaves**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug tst_nativevideoencoder && ctest --test-dir build/claude-debug -R tst_nativevideoencoder -V`
Expected: on macOS the stub is NOT linked (the VideoToolbox backend from Task 3 is — but Task 3 isn't written yet, so until then this links the stub via the `elseif(NOT WIN32)` only on non-Apple/non-Windows). On macOS, the `if(APPLE)` branch references `nativevideoencoder_videotoolbox.mm` which does not exist yet → expected link/compile failure naming the missing `.mm`. That is the correct RED for Task 3. To get a green Task 2 in isolation on macOS, temporarily point the APPLE test-lib at the stub, OR proceed directly to Task 3 and treat Tasks 2+3 as one commit. **Chosen approach:** commit Task 2 with the APPLE/WIN32 test-lib branches pointing at `nativevideoencoder_stub.cpp` (so the capability test passes: stub → caps.h264 false → create() returns null → test asserts null), then Task 3/4 switch the branch to the real backend.

Adjust Step 2's `tests/unit/CMakeLists.txt` so ALL platforms point `olr_test_nativevideoencoder` at `nativevideoencoder_stub.cpp` for now; Task 3 (macOS) and Task 4 (Windows) flip their branch to the real backend. With the stub, `capabilityProbeIsConsistentWithCreate` passes (false ↔ null) and `encodesIntraFramesWhenAvailable` `QSKIP`s.

Run again:
Expected: PASS (1 pass, 1 skip).

- [ ] **Step 6: Commit**

```bash
git add recorder_engine/codec/nativevideoencoder.h recorder_engine/codec/nativevideoencoder_stub.cpp tests/unit/tst_nativevideoencoder.cpp tests/unit/CMakeLists.txt CMakeLists.txt
git commit -m "feat: NativeVideoEncoder interface + stub + capability probe"
```

---

## Task 3: VideoToolbox H.264 encoder backend (macOS/iOS)

**Files:**
- Create: `recorder_engine/codec/nativevideoencoder_videotoolbox.mm`
- Modify: `tests/unit/CMakeLists.txt` (point the APPLE `olr_test_nativevideoencoder` at the `.mm`)

**Interfaces:**
- Consumes: `NativeVideoEncoder` (Task 2), `buildAvcCFromParameterSets` (Task 1).
- Produces: the macOS/iOS implementation of `NativeVideoEncoder::create`, `queryNativeVideoEncodeCapabilities`, `encode`, `flush`, `avccExtradata`.

> Template: this mirrors `recorder_engine/ingest/nativevideodecoder_videotoolbox.mm` in reverse (CVPixelBuffer ← AVFrame; VTCompressionSession; output callback produces a CMSampleBuffer whose CMBlockBuffer is already length-prefixed AVCC). All-intra: `MaxKeyFrameInterval = 1`, `AllowFrameReordering = false`, and each `EncodeFrame` forces a keyframe. SPS/PPS are read from the first encoded sample's `CMVideoFormatDescriptionGetH264ParameterSetAtIndex` and turned into `avcC` via Task 1.

- [ ] **Step 1: Switch the macOS test lib to the real backend**

In `tests/unit/CMakeLists.txt`, change the APPLE branch of `olr_test_nativevideoencoder` to compile `nativevideoencoder_videotoolbox.mm` (instead of the stub).

- [ ] **Step 2: Run the test to verify it fails**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug tst_nativevideoencoder`
Expected: FAIL — `nativevideoencoder_videotoolbox.mm` does not exist.

- [ ] **Step 3: Write the VideoToolbox backend**

Create `recorder_engine/codec/nativevideoencoder_videotoolbox.mm`:

```objcpp
#include "recorder_engine/codec/nativevideoencoder.h"
#include "recorder_engine/codec/avcc.h"

#ifdef __APPLE__

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

#include <QList>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace {

struct EncodedPacket {
    QByteArray data;
    int64_t ptsTicks = 0;
    bool keyframe = false;
};

// Copy a CPU YUV420P AVFrame into an I420 CVPixelBuffer.
CVPixelBufferRef makeI420PixelBuffer(const AVFrame* f) {
    CVPixelBufferRef pb = nullptr;
    const void* keys[] = {kCVPixelBufferIOSurfacePropertiesKey};
    const void* vals[] = {(__bridge const void*)@{}};
    CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault, keys, vals, 1,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    const CVReturn r = CVPixelBufferCreate(kCFAllocatorDefault, f->width, f->height,
                                           kCVPixelFormatType_420YpCbCr8Planar, attrs, &pb);
    if (attrs) CFRelease(attrs);
    if (r != kCVReturnSuccess || !pb) return nullptr;

    CVPixelBufferLockBaseAddress(pb, 0);
    for (int plane = 0; plane < 3; ++plane) {
        auto* dst = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pb, plane));
        const size_t dstStride = CVPixelBufferGetBytesPerRowOfPlane(pb, plane);
        const int rows = plane == 0 ? f->height : f->height / 2;
        const int bytes = plane == 0 ? f->width : f->width / 2;
        const uint8_t* src = f->data[plane];
        const int srcStride = f->linesize[plane];
        for (int y = 0; y < rows; ++y)
            memcpy(dst + y * dstStride, src + y * srcStride, bytes);
    }
    CVPixelBufferUnlockBaseAddress(pb, 0);
    return pb;
}

QByteArray extractAvcC(CMSampleBufferRef sample) {
    CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sample);
    if (!fmt) return {};
    size_t count = 0;
    if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, 0, nullptr, nullptr, &count, nullptr) != noErr)
        return {};
    QList<QByteArray> sps, pps;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* ps = nullptr;
        size_t psSize = 0;
        if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, i, &ps, &psSize, nullptr, nullptr) != noErr)
            continue;
        const QByteArray nal(reinterpret_cast<const char*>(ps), int(psSize));
        const int nalType = psSize > 0 ? (ps[0] & 0x1f) : 0;
        if (nalType == 7) sps.append(nal);       // SPS
        else if (nalType == 8) pps.append(nal);  // PPS
    }
    return buildAvcCFromParameterSets(sps, pps);
}

bool sampleIsKeyframe(CMSampleBufferRef sample) {
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (!attachments || CFArrayGetCount(attachments) == 0) return true; // no attachments → sync
    CFDictionaryRef dict = (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
    CFBooleanRef notSync = nullptr;
    if (CFDictionaryGetValueIfPresent(dict, kCMSampleAttachmentKey_NotSync, (const void**)&notSync) && notSync)
        return !CFBooleanGetValue(notSync);
    return true;
}

// CMBlockBuffer for H.264 is length-prefixed (AVCC) — exactly what MKV wants.
QByteArray copyBlockBuffer(CMSampleBufferRef sample) {
    CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sample);
    if (!bb) return {};
    size_t total = CMBlockBufferGetDataLength(bb);
    QByteArray out(int(total), Qt::Uninitialized);
    if (CMBlockBufferCopyDataBytes(bb, 0, total, out.data()) != kCMBlockBufferNoErr) return {};
    return out;
}

} // namespace

class VideoToolboxEncoder : public NativeVideoEncoder {
public:
    VTCompressionSessionRef session = nullptr;
    QByteArray avcc;
    std::vector<EncodedPacket>* sink = nullptr; // set per encode call

    ~VideoToolboxEncoder() override {
        if (session) {
            VTCompressionSessionInvalidate(session);
            CFRelease(session);
        }
    }

    bool encode(const AVFrame* frame, int64_t ptsTicks,
                const PacketCallback& onPacket, QString* error) override {
        std::vector<EncodedPacket> packets;
        sink = &packets;
        CVPixelBufferRef pb = makeI420PixelBuffer(frame);
        if (!pb) { if (error) *error = QStringLiteral("CVPixelBuffer alloc failed"); sink = nullptr; return false; }

        const CMTime pts = CMTimeMake(ptsTicks, 90000);
        const void* fk[] = {kVTEncodeFrameOptionKey_ForceKeyFrame};
        const void* fv[] = {kCFBooleanTrue};
        CFDictionaryRef frameProps = CFDictionaryCreate(kCFAllocatorDefault, fk, fv, 1,
                                                        &kCFTypeDictionaryKeyCallBacks,
                                                        &kCFTypeDictionaryValueCallBacks);
        const OSStatus st = VTCompressionSessionEncodeFrame(
            session, pb, pts, kCMTimeInvalid, frameProps,
            reinterpret_cast<void*>(ptsTicks), nullptr);
        if (frameProps) CFRelease(frameProps);
        CVPixelBufferRelease(pb);
        if (st != noErr) { if (error) *error = QStringLiteral("VTCompressionSessionEncodeFrame failed (%1)").arg(st); sink = nullptr; return false; }
        VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
        sink = nullptr;
        for (auto& p : packets) onPacket(p.data, p.ptsTicks, p.keyframe);
        return true;
    }

    bool flush(const PacketCallback&, QString*) override {
        // Synchronous CompleteFrames in encode() means nothing is buffered.
        return true;
    }

    QByteArray avccExtradata() const override { return avcc; }
};

static void compressionOutputCallback(void* outputRefCon, void* sourceFrameRefCon,
                                      OSStatus status, VTEncodeInfoFlags,
                                      CMSampleBufferRef sample) {
    auto* self = static_cast<VideoToolboxEncoder*>(outputRefCon);
    if (status != noErr || !sample || !self || !self->sink) return;
    if (self->avcc.isEmpty()) self->avcc = extractAvcC(sample);
    EncodedPacket p;
    p.data = copyBlockBuffer(sample);
    p.ptsTicks = reinterpret_cast<int64_t>(sourceFrameRefCon);
    p.keyframe = sampleIsKeyframe(sample);
    if (!p.data.isEmpty()) self->sink->push_back(std::move(p));
}

NativeVideoEncoder::~NativeVideoEncoder() = default;

std::unique_ptr<NativeVideoEncoder> NativeVideoEncoder::create(const Config& cfg, QString* error) {
    auto enc = std::unique_ptr<VideoToolboxEncoder>(new VideoToolboxEncoder());
    const void* ek[] = {kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder};
    const void* ev[] = {kCFBooleanTrue};
    CFDictionaryRef spec = CFDictionaryCreate(kCFAllocatorDefault, ek, ev, 1,
                                              &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks);
    OSStatus st = VTCompressionSessionCreate(kCFAllocatorDefault, cfg.width, cfg.height,
                                             kCMVideoCodecType_H264, spec, nullptr, nullptr,
                                             compressionOutputCallback, enc.get(), &enc->session);
    if (spec) CFRelease(spec);
    if (st != noErr || !enc->session) {
        if (error) *error = QStringLiteral("VTCompressionSessionCreate failed (%1)").arg(st);
        return nullptr;
    }
    VTSessionSetProperty(enc->session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    VTSessionSetProperty(enc->session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
    VTSessionSetProperty(enc->session, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_High_AutoLevel);
    const int one = 1;
    CFNumberRef kfi = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &one);
    VTSessionSetProperty(enc->session, kVTCompressionPropertyKey_MaxKeyFrameInterval, kfi);
    if (kfi) CFRelease(kfi);
    CFNumberRef br = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &cfg.bitrate);
    VTSessionSetProperty(enc->session, kVTCompressionPropertyKey_AverageBitRate, br);
    if (br) CFRelease(br);
    VTCompressionSessionPrepareToEncodeFrames(enc->session);
    return enc;
}

NativeVideoEncodeCapabilities queryNativeVideoEncodeCapabilities() {
    NativeVideoEncodeCapabilities caps;
    QString err;
    auto probe = NativeVideoEncoder::create({1280, 720, 30, 1, 4'000'000}, &err);
    caps.h264 = probe != nullptr;
    caps.detail = caps.h264 ? QStringLiteral("VideoToolbox H.264 encode available")
                            : QStringLiteral("VideoToolbox H.264 encode unavailable: %1").arg(err);
    return caps;
}

#endif // __APPLE__
```

- [ ] **Step 4: Run the test to verify it passes (macOS hardware)**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug tst_nativevideoencoder && ctest --test-dir build/claude-debug -R tst_nativevideoencoder -V`
Expected: PASS — `encodesIntraFramesWhenAvailable` runs (not skipped), all packets keyframes, avcC non-empty starting `0x01`.

- [ ] **Step 5: Commit**

```bash
git add recorder_engine/codec/nativevideoencoder_videotoolbox.mm tests/unit/CMakeLists.txt
git commit -m "feat: VideoToolbox H.264 encoder backend (all-intra)"
```

---

## Task 4: MediaFoundation H.264 encoder backend (Windows)

**Files:**
- Create: `recorder_engine/codec/nativevideoencoder_mediafoundation.cpp`
- Modify: `tests/unit/CMakeLists.txt` (point the WIN32 `olr_test_nativevideoencoder` at the `.cpp`)

**Interfaces:**
- Consumes: `NativeVideoEncoder` (Task 2), `buildAvcCFromParameterSets` (Task 1).
- Produces: the Windows implementation of the four `NativeVideoEncoder` entry points.

> Template: `recorder_engine/ingest/nativevideodecoder_mediafoundation.cpp` (ComPtr / IMFTransform / MFT enumeration). The H.264 encoder MFT (enumerated via `MFTEnumEx` with `MFT_CATEGORY_VIDEO_ENCODER` + `MFVideoFormat_H264`, hardware flag preferred) takes NV12 input and emits an H.264 elementary stream. Configure all-intra via `ICodecAPI`/`CODECAPI_AVEncMPVDefaultBPictureCount = 0` + `CODECAPI_AVEncMPVGOPSize = 1`. The MF encoder emits Annex B; convert to length-prefixed AVCC for the packet, and read SPS/PPS from `MF_MT_MPEG_SEQUENCE_HEADER` on the negotiated output type to build the avcC. **This backend can only be built/validated on Windows CI** (the dev machine is macOS) — the implementer notes that and relies on the CI Windows runner's `tst_nativevideoencoder`.

- [ ] **Step 1: Switch the Windows test lib to the real backend**

In `tests/unit/CMakeLists.txt`, change the WIN32 branch of `olr_test_nativevideoencoder` to compile `nativevideoencoder_mediafoundation.cpp`.

- [ ] **Step 2: Write the MediaFoundation backend**

Create `recorder_engine/codec/nativevideoencoder_mediafoundation.cpp` implementing the four entry points:

```cpp
#include "recorder_engine/codec/nativevideoencoder.h"
#include "recorder_engine/codec/avcc.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <QByteArray>
#include <QList>

#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

extern "C" {
#include <libavutil/frame.h>
}

using Microsoft::WRL::ComPtr;

namespace {

// Split an Annex B elementary stream into NAL payloads (no start codes).
QList<QByteArray> splitAnnexB(const uint8_t* data, int size) {
    QList<QByteArray> nals;
    int i = 0;
    auto startCode = [&](int p) -> int {
        if (p + 3 <= size && data[p] == 0 && data[p+1] == 0 && data[p+2] == 1) return 3;
        if (p + 4 <= size && data[p] == 0 && data[p+1] == 0 && data[p+2] == 0 && data[p+3] == 1) return 4;
        return 0;
    };
    QList<int> starts;
    for (int p = 0; p + 3 <= size;) { int s = startCode(p); if (s) { starts.append(p); p += s; } else ++p; }
    for (int k = 0; k < starts.size(); ++k) {
        const int off = starts[k] + startCode(starts[k]);
        const int end = (k + 1 < starts.size()) ? starts[k+1] : size;
        if (end > off) nals.append(QByteArray(reinterpret_cast<const char*>(data + off), end - off));
    }
    (void)i;
    return nals;
}

QByteArray annexBToLengthPrefixed(const QList<QByteArray>& nals) {
    QByteArray out;
    for (const QByteArray& nal : nals) {
        const quint32 n = quint32(nal.size());
        out.append(char((n >> 24) & 0xff)); out.append(char((n >> 16) & 0xff));
        out.append(char((n >> 8) & 0xff));  out.append(char(n & 0xff));
        out.append(nal);
    }
    return out;
}

} // namespace

// Full implementation: enumerate the H.264 encoder MFT (MFTEnumEx,
// MFT_CATEGORY_VIDEO_ENCODER, MFVideoFormat_H264, prefer
// MFT_ENUM_FLAG_HARDWARE), set the output type (MFVideoFormat_H264 + frame
// size/rate + AverageBitrate + eAVEncH264VProfile_High), set the input type
// (MFVideoFormat_NV12 + frame size/rate), configure all-intra via ICodecAPI
// (CODECAPI_AVEncMPVGOPSize=1, CODECAPI_AVEncMPVDefaultBPictureCount=0,
// CODECAPI_AVEncCommonRateControlMode=eAVEncCommonRateControlMode_CBR,
// CODECAPI_AVLowLatencyMode=true), then per encode(): convert the AVFrame
// (YUV420P) to NV12, wrap in an IMFSample (MFCreateMemoryBuffer +
// MFCreateSample), ProcessInput, drain ProcessOutput, convert each Annex-B
// output to length-prefixed AVCC, mark keyframe via
// MFSampleExtension_CleanPoint, and on the first output read
// MF_MT_MPEG_SEQUENCE_HEADER from the encoder's output media type to build
// the avcC via buildAvcCFromParameterSets (split the sequence header's
// Annex-B SPS/PPS). create() returns nullptr if MFTEnumEx finds no encoder or
// MFStartup/SetOutputType fails. queryNativeVideoEncodeCapabilities() probes
// create() at 1280x720 and reports the result.

NativeVideoEncoder::~NativeVideoEncoder() = default;
```

> The implementer fills in the full class body and the four entry points per the comment, modeling COM lifetime/MFStartup handling on the existing MediaFoundation decoder. Keep `create()` strictly hardware-or-null.

- [ ] **Step 3: Build + test (Windows CI)**

On the Windows CI runner: `ninja -C build tst_nativevideoencoder` then `ctest -R tst_nativevideoencoder -V`.
Expected: PASS — `encodesIntraFramesWhenAvailable` runs on a HW-capable runner (or QSKIPs where no MFT is present), all packets keyframes, avcC non-empty.
On macOS this file is not compiled (the APPLE branch uses the VideoToolbox lib) — verify the macOS build is unaffected: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug tst_nativevideoencoder` still PASSES.

- [ ] **Step 4: Commit**

```bash
git add recorder_engine/codec/nativevideoencoder_mediafoundation.cpp tests/unit/CMakeLists.txt
git commit -m "feat: MediaFoundation H.264 encoder backend (all-intra)"
```

---

## Task 5: Harden the muxer H.264 extradata path (Plan 1 carry-forward)

**Files:**
- Modify: `recorder_engine/muxer.cpp` (the H.264 extradata block from Plan 1 Task 3)
- Test: `tests/unit/tst_muxer.cpp`

**Interfaces:**
- Consumes: `Muxer::init(..., VideoCodecChoice codec, const QByteArray& videoExtradata)` (Plan 1).
- Produces: `init()` now FAILS (returns false, logs) when `codec == H264Hardware` and either `videoExtradata` is empty or its `av_mallocz` fails — never a half-configured H.264 stream that returns success.

> Carry-forward from Plan 1: previously an OOM (or empty extradata) on the H.264 path left `codec_id = H264` with no extradata and still returned success → an unplayable MKV. Now that the H.264 path is live, this must fail loudly.

- [ ] **Step 1: Write the failing test**

In `tests/unit/tst_muxer.cpp`, add a slot declaration and body:

```cpp
    void initFailsForH264WithoutExtradata();
```

```cpp
void TestMuxer::initFailsForH264WithoutExtradata() {
    Muxer m;
    m.setOutputDirectory(m_home.path());
    const QStringList names{QStringLiteral("A")};
    // H.264 requires avcC extradata; empty must be rejected, not silently accepted.
    QVERIFY(!m.init(QStringLiteral("olr_unit_h264_noextradata"), 1, 320, 240, 30, names,
                    48000, 2, VideoCodecChoice::H264Hardware, QByteArray()));
}
```

Add the include if not present: `#include "recorder_engine/codec/videocodecchoice.h"` (already transitively via muxer.h).

- [ ] **Step 2: Run test to verify it fails**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug tst_muxer && ctest --test-dir build/claude-debug -R tst_muxer -V`
Expected: FAIL — current code returns true for H.264 + empty extradata.

- [ ] **Step 3: Write the implementation**

In `recorder_engine/muxer.cpp`, in the per-stream loop where the codec_id is set (Plan 1 Task 3 block), replace the H.264 branch so empty extradata or a failed allocation aborts init. Add, immediately after computing `codec`, a single up-front guard before the stream loop:

```cpp
    if (codec == VideoCodecChoice::H264Hardware && videoExtradata.isEmpty()) {
        qWarning() << "Muxer: H.264 selected but no avcC extradata provided; refusing to init.";
        avformat_free_context(m_outCtx);
        m_outCtx = nullptr;
        resetTelemetryTracks();
        return false;
    }
```

(Place it just after `m_outCtx` is allocated and the early `if (!m_outCtx) return false;`, before the stream-creation loop. `resetTelemetryTracks` is the existing local lambda.) In the per-stream extradata attach, if `av_mallocz` returns null, also fail:

```cpp
        if (codec == VideoCodecChoice::H264Hardware) {
            st->codecpar->extradata = static_cast<uint8_t*>(
                av_mallocz(videoExtradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
            if (!st->codecpar->extradata) {
                qWarning() << "Muxer: failed to allocate H.264 extradata.";
                avformat_free_context(m_outCtx);
                m_outCtx = nullptr;
                resetTelemetryTracks();
                return false;
            }
            memcpy(st->codecpar->extradata, videoExtradata.constData(), videoExtradata.size());
            st->codecpar->extradata_size = videoExtradata.size();
        }
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug tst_muxer && ctest --test-dir build/claude-debug -R tst_muxer -V`
Expected: PASS — including `initFailsForH264WithoutExtradata` and all existing tests (default MPEG-2 unaffected).

- [ ] **Step 5: Commit**

```bash
git add recorder_engine/muxer.cpp tests/unit/tst_muxer.cpp
git commit -m "fix: Muxer rejects H.264 init without valid avcC extradata"
```

---

## Task 6: Wire the native encoder into the record path

**Files:**
- Modify: `recorder_engine/streamworker.h/.cpp` (own a `NativeVideoEncoder`; swap the encode block)
- Modify: `recorder_engine/replaymanager.h/.cpp` (blue encoder via `NativeVideoEncoder`; priming-encode → avcC → reordered muxer init)
- Test: `tests/unit/tst_h264_roundtrip.cpp`

**Interfaces:**
- Consumes: `NativeVideoEncoder` (Tasks 2-4), `Muxer::init(..., codec, extradata)` (Plan 1 + Task 5).
- Produces: a recorded `.mkv` whose video streams are `AV_CODEC_ID_H264`, intra-only, with valid `avcC` CodecPrivate, when `VideoCodecChoice::H264Hardware` is selected.

> Record-path reordering: today `startRecording()` calls `m_muxer->init(...)` (writes the header) THEN `setupBlueEncoder()`. For H.264 the avcC must exist BEFORE the header. New order for H.264: build the blue `NativeVideoEncoder`, priming-encode the blue frame to obtain `avccExtradata()`, THEN `m_muxer->init(..., H264Hardware, avcc)`. For MPEG-2 the order is unchanged. The per-source `StreamWorker` builds its own `NativeVideoEncoder` in `setupEncoder` and the encode tick uses it.

- [ ] **Step 1: Write the failing round-trip test**

Create `tests/unit/tst_h264_roundtrip.cpp` — encode several frames through a `NativeVideoEncoder`, mux them via `Muxer` with the avcC, then demux with FFmpeg and assert codec/intra/frame-count:

```cpp
// End-to-end avcC + muxing round-trip: native-encode grey frames, attach the
// encoder's avcC to the muxer, write a real MKV, then demux it and assert the
// stream is H.264, every frame is a keyframe, and the frame count matches.
#include <QtTest>
#include <QTemporaryDir>
#include <QScopeGuard>

#include "recorder_engine/muxer.h"
#include "recorder_engine/codec/nativevideoencoder.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
}

class TestH264RoundTrip : public QObject {
    Q_OBJECT
private slots:
    void encodeMuxDemuxYieldsIntraH264();
private:
    QTemporaryDir m_home;
};

void TestH264RoundTrip::encodeMuxDemuxYieldsIntraH264() {
    QString err;
    auto enc = NativeVideoEncoder::create({640, 480, 30, 1, 4'000'000}, &err);
    if (!enc) QSKIP("no hardware H.264 encoder on this platform");

    // Prime to obtain avcC.
    auto grey = []() {
        AVFrame* f = av_frame_alloc();
        f->format = AV_PIX_FMT_YUV420P; f->width = 640; f->height = 480;
        av_frame_get_buffer(f, 32);
        memset(f->data[0], 128, f->linesize[0] * 480);
        memset(f->data[1], 128, f->linesize[1] * 240);
        memset(f->data[2], 128, f->linesize[2] * 240);
        return f;
    };
    AVFrame* prime = grey();
    enc->encode(prime, 0, [](const QByteArray&, int64_t, bool){}, &err);
    av_frame_free(&prime);
    const QByteArray avcc = enc->avccExtradata();
    QVERIFY(!avcc.isEmpty());

    Muxer m;
    m.setOutputDirectory(m_home.path());
    const QStringList names{QStringLiteral("A")};
    QVERIFY(m.init(QStringLiteral("olr_h264_rt"), 1, 640, 480, 30, names,
                   48000, 2, VideoCodecChoice::H264Hardware, avcc));

    AVStream* st = m.getStream(0);
    QVERIFY(st);
    int written = 0;
    for (int i = 1; i <= 6; ++i) {
        AVFrame* f = grey();
        enc->encode(f, i, [&](const QByteArray& data, int64_t pts, bool key) {
            AVPacket* pkt = av_packet_alloc();
            av_new_packet(pkt, data.size());
            memcpy(pkt->data, data.constData(), data.size());
            pkt->stream_index = 0;
            pkt->pts = pkt->dts = av_rescale_q(pts, AVRational{1, 30}, st->time_base);
            pkt->duration = av_rescale_q(1, AVRational{1, 30}, st->time_base);
            if (key) pkt->flags |= AV_PKT_FLAG_KEY;
            m.writePacket(pkt);
            av_packet_free(&pkt);
            ++written;
        }, &err);
        av_frame_free(&f);
    }
    m.close();
    QVERIFY(written >= 6);

    const QString path = m_home.path() + "/olr_h264_rt.mkv";
    AVFormatContext* ctx = nullptr;
    QVERIFY(avformat_open_input(&ctx, path.toUtf8().constData(), nullptr, nullptr) >= 0);
    auto closeInput = qScopeGuard([&] { avformat_close_input(&ctx); });
    QVERIFY(avformat_find_stream_info(ctx, nullptr) >= 0);

    int videoIdx = -1;
    for (unsigned i = 0; i < ctx->nb_streams; ++i)
        if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { videoIdx = int(i); break; }
    QVERIFY(videoIdx >= 0);
    QCOMPARE(ctx->streams[videoIdx]->codecpar->codec_id, AV_CODEC_ID_H264);
    QVERIFY(ctx->streams[videoIdx]->codecpar->extradata_size > 0);

    int frames = 0, keyframes = 0;
    AVPacket* pkt = av_packet_alloc();
    auto freePkt = qScopeGuard([&] { av_packet_free(&pkt); });
    while (av_read_frame(ctx, pkt) >= 0) {
        if (pkt->stream_index == videoIdx) {
            ++frames;
            if (pkt->flags & AV_PKT_FLAG_KEY) ++keyframes;
        }
        av_packet_unref(pkt);
    }
    QCOMPARE(frames, keyframes); // all-intra
    QVERIFY(frames >= 6);
}

QTEST_GUILESS_MAIN(TestH264RoundTrip)
#include "tst_h264_roundtrip.moc"
```

Register in `tests/unit/CMakeLists.txt`: `olr_add_unit_test(tst_h264_roundtrip olr_test_engine)` and link the per-platform `olr_test_nativevideoencoder` (same `if(APPLE)/elseif(WIN32)/else` pattern as Task 2, or factor a helper — simplest: `target_link_libraries(tst_h264_roundtrip PRIVATE olr_test_nativevideoencoder)`).

- [ ] **Step 2: Run test to verify it fails**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug tst_h264_roundtrip && ctest --test-dir build/claude-debug -R tst_h264_roundtrip -V`
Expected: PASS already if Tasks 1-5 are correct (this test only uses the encoder + muxer, which exist). If it fails, the failure pinpoints an avcC/muxing bug. (This test is the behavioral gate for the encoder→muxer contract; it does not need StreamWorker.)

- [ ] **Step 3: Wire StreamWorker's encode path**

In `streamworker.h`, add `#include "recorder_engine/codec/nativevideoencoder.h"`, a member `std::unique_ptr<NativeVideoEncoder> m_nativeEncoder;`, and keep `m_persistentEncCtx` for MPEG-2.

In `streamworker.cpp` `setupEncoder`, replace the Plan-1 H.264 guard (`return false`) with: build `m_nativeEncoder = NativeVideoEncoder::create({m_targetWidth, m_targetHeight, m_targetFps, 1, 30'000'000}, &err)`; if null, `qWarning` + return false (hardware-only contract).

In the encode tick (`processEncoderTick`, the block at the `avcodec_send_frame` site), branch on `m_videoCodec`: for MPEG-2 keep the existing `avcodec_send_frame`/`avcodec_receive_packet`; for H.264 call `m_nativeEncoder->encode(m_latestFrame, m_internalFrameCount, onPacket, &err)` where `onPacket` allocates an `AVPacket`, copies `data`, sets `stream_index = track`, `pts = dts = av_rescale_q(ptsTicks, {1, m_targetFps}, st->time_base)`, `duration = 1`-equivalent, sets `AV_PKT_FLAG_KEY` when `keyframe`, and calls `m_muxer->writePacket(pkt)` then frees it. Write the per-frame metadata exactly as the MPEG-2 path does.

- [ ] **Step 4: Wire ReplayManager blue encoder + priming + reordered init**

In `replaymanager.h`, add `#include "recorder_engine/codec/nativevideoencoder.h"` and `std::unique_ptr<NativeVideoEncoder> m_blueNativeEncoder;`.

In `replaymanager.cpp` `setupBlueEncoder`, replace the Plan-1 H.264 guard with: create `m_blueNativeEncoder`; priming-encode the existing blue `m_blueFrame` once; capture the produced packet bytes into `m_cachedBluePkt` (as an `AVPacket` with `AV_PKT_FLAG_KEY`), and stash `m_blueNativeEncoder->avccExtradata()` into a new member `QByteArray m_videoExtradata;`. For MPEG-2 the existing path stays and leaves `m_videoExtradata` empty.

In `startRecording()`, reorder for H.264: call `setupBlueEncoder()` BEFORE `m_muxer->init(...)` when `m_videoCodec == H264Hardware`, and pass `m_videoCodec, m_videoExtradata` to `init`. For MPEG-2, keep the current order (init then blue encoder) and pass `m_videoCodec` with an empty extradata. (Both arms call `init` with the codec; only the ordering and extradata differ.) On any failure, clean up symmetrically (close muxer / cleanup blue encoder) exactly as the current error paths do.

- [ ] **Step 5: Build, run unit suite + the round-trip**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug && ctest --test-dir build/claude-debug -L unit`
Expected: PASS — full build clean, all unit tests green (MPEG-2 path unchanged; H.264 round-trip green on macOS).

- [ ] **Step 6: Commit**

```bash
git add recorder_engine/streamworker.h recorder_engine/streamworker.cpp recorder_engine/replaymanager.h recorder_engine/replaymanager.cpp tests/unit/tst_h264_roundtrip.cpp tests/unit/CMakeLists.txt
git commit -m "feat: record hardware H.264 (priming-encode avcC + native encode tick)"
```

---

## Task 7: Hardware H.264 playback decode

**Files:**
- Modify: `playback/playbackworker.h/.cpp` (route H.264 decoder-bank tracks through `NativeVideoDecoder`)
- Test: `tests/unit/tst_h264_roundtrip.cpp` (extend: decode the muxed H.264 back via `NativeVideoDecoder` and assert frame dimensions)

**Interfaces:**
- Consumes: existing `NativeVideoDecoder` (`recorder_engine/ingest/nativevideodecoder.h`), `CompressedAccessUnit`, `H26xParameterSets`.
- Produces: H.264 file tracks decode on hardware (`NativeVideoDecoder`); MPEG-2 tracks keep FFmpeg software decode.

> The playback decoder bank (`m_decoderBank`, each with an FFmpeg `codecCtx`) decodes per track. For an H.264 track, instead of opening an FFmpeg `codecCtx`, attach a `NativeVideoDecoder` and parse the file's `extradata` (avcC) into `H26xParameterSets` (SPS/PPS) once, then feed each packet's payload (convert avcC length-prefixed → the access unit the decoder expects) to `NativeVideoDecoder::decode`, producing the same `AVFrame` the bank consumes. MPEG-2 stays on FFmpeg.

- [ ] **Step 1: Extend the round-trip test to decode via NativeVideoDecoder**

In `tst_h264_roundtrip.cpp`, after the demux assertions, add a decode pass: build a `NativeVideoDecoder(640, 480)`, parse the stream `extradata` (avcC) into SPS/PPS, feed the read packets as `CompressedAccessUnit`s, and assert at least one `AVFrame` comes back at 640×480. Guard the whole decode pass with the same `QSKIP` if `queryNativeVideoDecodeCapabilities().h264` is false.

```cpp
    // (append inside encodeMuxDemuxYieldsIntraH264, after the demux loop)
    if (queryNativeVideoDecodeCapabilities().h264) {
        // Parse avcC → SPS/PPS, decode the first video packet, expect a 640x480 frame.
        // (Use the project's avcC parse helper or inline parse: skip 6-byte header,
        //  read numSPS, sized SPS NALs, numPPS, sized PPS NALs.)
        // Assert the decoded AVFrame width/height == 640/480.
    }
```

(The implementer writes the concrete avcC parse + a single-packet decode using `NativeVideoDecoder::decode`, asserting `frame->width == 640 && frame->height == 480`. Include `recorder_engine/ingest/nativevideodecoder.h`.)

- [ ] **Step 2: Run test to verify it fails**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug tst_h264_roundtrip && ctest --test-dir build/claude-debug -R tst_h264_roundtrip -V`
Expected: FAIL (or compile error) until the decode-pass assertions are satisfied by a correct avcC parse + decode call.

- [ ] **Step 3: Implement the decode pass in the test, then wire PlaybackWorker**

Make the test pass first (proves the decode contract). Then in `playbackworker.cpp`, where the decoder bank opens an FFmpeg decoder per track (the `clearDecoders`/bank-setup region near `run()`), branch: when `codecpar->codec_id == AV_CODEC_ID_H264`, attach a `NativeVideoDecoder` + parsed `H26xParameterSets` to that track instead of an FFmpeg `codecCtx`, and in `decodePacketIntoBank` route H.264 packets to `NativeVideoDecoder::decode` (producing the `AVFrame` the bank stores). MPEG-2 tracks are unchanged. Add a `std::unique_ptr<NativeVideoDecoder>` (+ parsed parameter sets) to the per-track bank struct.

- [ ] **Step 4: Build + run the playback + unit suite**

Run: `$HOME/Qt/Tools/Ninja/ninja -C build/claude-debug && ctest --test-dir build/claude-debug -L unit`
Expected: PASS — H.264 round-trip (encode→mux→demux→hardware-decode) green; MPEG-2 playback unaffected.

- [ ] **Step 5: Commit**

```bash
git add playback/playbackworker.h playback/playbackworker.cpp tests/unit/tst_h264_roundtrip.cpp
git commit -m "feat: hardware H.264 playback decode via NativeVideoDecoder"
```

---

## Task 8: End-to-end headless H.264 recording check

**Files:**
- Modify: the headless E2E recording harness/test under `tests/e2e/` (mirror the existing MPEG-2 recording smoke)
- Test: a new ctest case (e.g. `e2e_h264_record_smoke`) gated to platforms with HW H.264

**Interfaces:**
- Consumes: the full record path (ReplayManager → StreamWorker → NativeVideoEncoder → Muxer).
- Produces: proof that a real recording session with `VideoCodecChoice::H264Hardware` yields a playable, intra-only H.264 MKV with the expected track layout.

> Reuse the existing E2E recording harness (see `tests/e2e/`). Run a short session with the codec set to H.264, then assert with FFmpeg/ffprobe: the video stream is `AV_CODEC_ID_H264`, every video packet is a keyframe, the track layout (video + per-view audio + metadata + telemetry) is intact, and the content is non-trivial (not blue-fill-only — content-check per the SRT smoke lesson). Gate the test to skip where `queryNativeVideoEncodeCapabilities().h264` is false.

- [ ] **Step 1: Add the E2E case**

Add an H.264 variant of the existing recording E2E (set codec via the harness's settings/ReplayManager `setVideoCodec(VideoCodecChoice::H264Hardware)`), recording a few seconds of a synthetic source.

- [ ] **Step 2: Run it**

Run: `ctest --test-dir build/claude-debug -R e2e_h264_record_smoke -V`
Expected: PASS where HW H.264 exists; SKIP otherwise.

- [ ] **Step 3: Assert output properties**

Confirm via ffprobe/libav in the test: `codec_id == h264`, all video packets keyframes, full track layout present, audio content present.

- [ ] **Step 4: Commit**

```bash
git add tests/e2e
git commit -m "test: end-to-end hardware H.264 recording smoke"
```

---

## Self-Review

**Spec coverage (this plan = subsystem 2 of the design doc):**
- Native `NativeVideoEncoder` + VideoToolbox + MediaFoundation + stub → Tasks 2, 3, 4. ✓
- `queryNativeVideoEncodeCapabilities` (capability probe) → Tasks 2 (stub) / 3 (VT) / 4 (MF). ✓
- avcC bridging (priming encode → extradata before header) → Tasks 1, 6. ✓
- Record path wired (StreamWorker + blue encoder + reordered init) → Task 6. ✓
- Hardware-only enforcement (create→null, no software fallback) → Tasks 2-4 + StreamWorker/blue guards (Task 6). ✓
- Plan 1 carry-forward: extradata-OOM / empty hardening → Task 5. ✓
- Hardware H.264 playback decode (reuse NativeVideoDecoder) → Task 7. ✓
- avcC round-trip + E2E codec round-trip + intra/frame-accuracy verification → Tasks 1, 6, 7, 8. ✓
- All-intra, 4:2:0 8-bit High, PTS==DTS → encoder config in Tasks 3, 4; verified in 6, 8. ✓
- Benchmark engine, settings/UI + gating → **out of scope** (Plans 3, 4). Not gaps.
- Plan 1 carry-forward #2 (videoCodecToString switch) → only fires when a 3rd codec is added; not in this plan. Correctly still deferred.

**Placeholder scan:** Tasks 1-3, 5-6 contain complete code. Task 4 (MediaFoundation) and the Task 7 PlaybackWorker wiring are described as concrete implementations with the full algorithm specified but the long COM/bank bodies left for the implementer to write against the named template files (`nativevideodecoder_mediafoundation.cpp`, the decoder bank) — these are platform bodies too large and machine-specific to inline byte-perfect, and are gated by their behavioral tests (Tasks 4, 7). This is the intended granularity for native-API tasks, not a TBD: every entry point, property, and conversion is named with exact API calls.

**Type consistency:** `NativeVideoEncoder::Config{width,height,fpsNum,fpsDen,bitrate}`, `PacketCallback(const QByteArray&, int64_t, bool)`, `create`/`encode`/`flush`/`avccExtradata` are identical across Tasks 2-4, 6, and the tests. `buildAvcCFromParameterSets(QList<QByteArray> sps, QList<QByteArray> pps)` (Task 1) is used identically in Tasks 3, 4. `Muxer::init(..., VideoCodecChoice, const QByteArray&)` matches Plan 1 and is called consistently in Tasks 5, 6 and the round-trip test.

**Execution note:** Task 4 (Windows) and the CI side of Task 8 can only be validated on the Windows runner; on the macOS dev machine they compile-guard out and their tests `QSKIP`. The implementer must flag if the Windows CI run surfaces MFT issues that need a Windows-capable iteration.
