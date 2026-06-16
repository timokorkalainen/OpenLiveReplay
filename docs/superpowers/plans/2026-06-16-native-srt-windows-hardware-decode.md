# Native SRT Windows Hardware Decode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add Windows native SRT H.264/H.265 hardware decoding through Media Foundation and D3D11 while preserving the existing Apple VideoToolbox native SRT path and FFmpeg fallback.

**Architecture:** Rename the Apple-specific `VideoToolboxDecoder` boundary to `NativeVideoDecoder`, keep `NativeSrtIngestSession` as the shared SRT/MPEG-TS/H.26x orchestrator, and add a Windows `NativeVideoDecoder` implementation backed by Media Foundation decoder MFTs with a D3D11/DXGI device manager. Reuse the existing SRT e2e scripts for both Apple and Windows native labels; add HEVC coverage separately because the current scripts generate H.264.

**Tech Stack:** C++17, Qt 6, CMake, QtTest, FFmpeg test harnesses, libsrt, Apple VideoToolbox/CoreMedia/CoreVideo, Windows Media Foundation, D3D11/DXGI, Winsock.

**Spec:** `docs/superpowers/specs/2026-06-16-native-srt-windows-hardware-decode-design.md`

---

## Before You Start

- Work in the isolated worktree:
  ```bash
  cd /Users/timo.korkalainen/Development/timo/OpenLiveReplay/.worktrees/windows-native-srt-decode
  ```
- Build inside the worktree, not `/tmp`, because Qt `moc` generated relative includes incorrectly when this source tree under `.worktrees/` was built from `/tmp`.
- Baseline macOS test command:
  ```bash
  cmake -S . -B build/baseline -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos
  cmake --build build/baseline --target tst_ingestbackendselector -j8
  ctest --test-dir build/baseline -R tst_ingestbackendselector --output-on-failure
  ```
- Windows configure command, once a Windows machine with Qt and dependencies is available:
  ```powershell
  cmake -S . -B build/windows-native -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH="$env:USERPROFILE\Qt\6.10.1\msvc2022_64" -DOLR_SRT_ROOT="C:\deps\srt"
  cmake --build build/windows-native --config Debug --target tst_nativevideodecoder
  ctest --test-dir build/windows-native -C Debug -R tst_nativevideodecoder --output-on-failure
  ```

## File Map

- `recorder_engine/ingest/nativevideodecoder.h`: platform-neutral decoder API, capabilities struct, and `queryNativeVideoDecodeCapabilities()`.
- `recorder_engine/ingest/nativevideodecoder_videotoolbox.mm`: renamed Apple implementation from `videotoolboxdecoder.mm`.
- `recorder_engine/ingest/nativevideodecoder_mediafoundation.cpp`: Windows Media Foundation/D3D11 implementation.
- `recorder_engine/ingest/nativevideodecoder_stub.cpp`: non-Apple/non-Windows fallback implementation and tests.
- `recorder_engine/ingest/nativeframecopy.h/.cpp`: CPU frame copy helpers for NV12/I420/P010-to-YUV420P conversions that can be unit tested without Media Foundation.
- `recorder_engine/ingest/nativesrtaddress.h/.cpp`: numeric IPv4 helper that hides POSIX vs Winsock header differences.
- `recorder_engine/ingest/nativesrtingestsession.h/.cpp`: shared native SRT session, updated to use `NativeVideoDecoder`, native fallback reason, and address helper.
- `recorder_engine/ingest/ingestsession.h`: add optional fallback reason API.
- `recorder_engine/streamworker.cpp`: choose FFmpeg on the next retry after a native decode-capability failure.
- `tests/unit/tst_nativevideodecoder.cpp`: API/capability tests that run on all platforms.
- `tests/unit/tst_nativeframecopy.cpp`: stride and pixel-layout tests for copy helpers.
- `tests/unit/tst_nativesrtaddress.cpp`: numeric IPv4 validation tests.
- `tests/e2e/CMakeLists.txt`: shared native SRT e2e registration helper, Apple and Windows labels.
- `tests/e2e/run_srt_hevc_smoke.sh`: optional HEVC SRT smoke producer, capability-gated.
- `tests/e2e/SRT_README.md`: document shared native labels and Windows notes.
- `CMakeLists.txt`: platform source selection, Windows Media Foundation/D3D11/Winsock/libsrt linkage.
- `tests/CMakeLists.txt`: test library source selection and Windows linkage.

---

### Task 1: Add NativeVideoDecoder API and Stub Test

**Files:**
- Create: `recorder_engine/ingest/nativevideodecoder.h`
- Create: `tests/unit/tst_nativevideodecoder.cpp`
- Modify: `tests/unit/CMakeLists.txt`

- [ ] **Step 1: Write the failing unit test**

Create `tests/unit/tst_nativevideodecoder.cpp`:

```cpp
#include <QtTest>

#include "recorder_engine/ingest/nativevideodecoder.h"

class TestNativeVideoDecoder : public QObject {
    Q_OBJECT
private slots:
    void defaultCapabilitiesAreFalse();
};

void TestNativeVideoDecoder::defaultCapabilitiesAreFalse() {
    const NativeVideoDecodeCapabilities caps;
    QVERIFY(!caps.h264);
    QVERIFY(!caps.hevc);
    QVERIFY(!caps.d3d11);
    QVERIFY(caps.detail.isEmpty());
}

QTEST_GUILESS_MAIN(TestNativeVideoDecoder)
#include "tst_nativevideodecoder.moc"
```

- [ ] **Step 2: Register the test and verify the expected failure**

In `tests/unit/CMakeLists.txt`, add:

```cmake
olr_add_unit_test(tst_nativevideodecoder olr_test_core)
```

Run:

```bash
cmake -S . -B build/windows-plan -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos
cmake --build build/windows-plan --target tst_nativevideodecoder -j8
```

Expected: compile failure because `recorder_engine/ingest/nativevideodecoder.h` does not exist.

- [ ] **Step 3: Add the decoder API header**

Create `recorder_engine/ingest/nativevideodecoder.h`:

```cpp
#ifndef NATIVEVIDEODECODER_H
#define NATIVEVIDEODECODER_H

#include "h26xaccessunit.h"

#include <QString>

#include <functional>

extern "C" {
#include <libavutil/frame.h>
}

struct NativeVideoDecodeCapabilities {
    bool h264 = false;
    bool hevc = false;
    bool d3d11 = false;
    QString detail;
};

class NativeVideoDecoder {
public:
    using FrameCallback = std::function<void(AVFrame*)>;

    NativeVideoDecoder(int outputWidth, int outputHeight);
    ~NativeVideoDecoder();

    NativeVideoDecoder(const NativeVideoDecoder&) = delete;
    NativeVideoDecoder& operator=(const NativeVideoDecoder&) = delete;

    bool decode(const CompressedAccessUnit& unit, FrameCallback onFrame, QString* error);
    void reset();

private:
    class Impl;
    Impl* m_impl = nullptr;
};

NativeVideoDecodeCapabilities queryNativeVideoDecodeCapabilities();

#endif // NATIVEVIDEODECODER_H
```

- [ ] **Step 4: Verify the API test target builds**

Run:

```bash
cmake --build build/windows-plan --target tst_nativevideodecoder -j8
ctest --test-dir build/windows-plan -R tst_nativevideodecoder --output-on-failure
```

Expected on macOS: build succeeds. This task only proves the header contract; Task 2 wires the Apple implementation and Task 7 wires the Windows implementation.

- [ ] **Step 5: Commit**

```bash
git add recorder_engine/ingest/nativevideodecoder.h tests/unit/tst_nativevideodecoder.cpp tests/unit/CMakeLists.txt
git commit -m "test(ingest): add native video decoder API contract"
```

---

### Task 2: Rename VideoToolboxDecoder to NativeVideoDecoder

**Files:**
- Delete: `recorder_engine/ingest/videotoolboxdecoder.h`
- Move: `recorder_engine/ingest/videotoolboxdecoder.mm` -> `recorder_engine/ingest/nativevideodecoder_videotoolbox.mm`
- Move: `recorder_engine/ingest/videotoolboxdecoder_stub.cpp` -> `recorder_engine/ingest/nativevideodecoder_stub.cpp`
- Modify: `recorder_engine/ingest/nativevideodecoder.h`
- Modify: `recorder_engine/ingest/nativesrtingestsession.h`
- Modify: `recorder_engine/ingest/nativesrtingestsession.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Move implementation files and remove the old header**

Run:

```bash
git mv recorder_engine/ingest/videotoolboxdecoder.mm recorder_engine/ingest/nativevideodecoder_videotoolbox.mm
git mv recorder_engine/ingest/videotoolboxdecoder_stub.cpp recorder_engine/ingest/nativevideodecoder_stub.cpp
git rm recorder_engine/ingest/videotoolboxdecoder.h
```

Task 1 already created `nativevideodecoder.h`, so do not move the old header over
it. Compare the old header before removing it and copy any missing public API
into `nativevideodecoder.h`; at the time this plan was written, the public API
matches except for the class name and capability struct.

- [ ] **Step 2: Rename symbols**

In the moved files and all includes:

```text
VideoToolboxDecoder -> NativeVideoDecoder
videotoolboxdecoder.h -> nativevideodecoder.h
videotoolboxdecoder.mm -> nativevideodecoder_videotoolbox.mm
videotoolboxdecoder_stub.cpp -> nativevideodecoder_stub.cpp
```

Also update user-facing error text that names `VideoToolbox` only where the boundary is platform-neutral. Keep Apple-specific internal error messages such as `VideoToolbox decompression session creation failed`.

- [ ] **Step 3: Add Apple capability query**

At the bottom of `nativevideodecoder_videotoolbox.mm`, before `#endif`, add:

```cpp
NativeVideoDecodeCapabilities queryNativeVideoDecodeCapabilities() {
    NativeVideoDecodeCapabilities caps;
    caps.h264 = true;
    caps.hevc = true;
    caps.detail = QStringLiteral("VideoToolbox native decode available");
    return caps;
}
```

- [ ] **Step 4: Update CMake source names**

In `CMakeLists.txt`, replace the old source names:

```cmake
recorder_engine/ingest/videotoolboxdecoder.h
recorder_engine/ingest/videotoolboxdecoder.mm
recorder_engine/ingest/videotoolboxdecoder_stub.cpp
```

For Apple:

```cmake
target_sources(OpenLiveReplay PRIVATE
    recorder_engine/ingest/nativevideodecoder_videotoolbox.mm
    recorder_engine/ingest/nativesrtingestsession.cpp
    recorder_engine/ingest/audiotoolboxaacdecoder.mm)
```

For non-Apple/non-Windows:

```cmake
target_sources(OpenLiveReplay PRIVATE recorder_engine/ingest/nativevideodecoder_stub.cpp)
```

In `tests/CMakeLists.txt`, use the same renamed files for `olr_test_engine`.

- [ ] **Step 5: Verify Apple behavior still builds**

Run:

```bash
cmake -S . -B build/windows-plan -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos
cmake --build build/windows-plan --target tst_ingestbackendselector tst_nativevideodecoder -j8
ctest --test-dir build/windows-plan -R "tst_ingestbackendselector|tst_nativevideodecoder" --output-on-failure
```

Expected: both tests pass or platform-specific native decode runtime assertion skips on macOS.

- [ ] **Step 6: Commit**

```bash
git add recorder_engine/ingest CMakeLists.txt tests/CMakeLists.txt tests/unit
git commit -m "refactor(ingest): rename native video decoder boundary"
```

---

### Task 3: Share Native SRT E2E Registration

**Files:**
- Modify: `tests/e2e/CMakeLists.txt`
- Modify: `tests/e2e/SRT_README.md`

- [ ] **Step 1: Refactor the existing Apple registration into a helper**

In `tests/e2e/CMakeLists.txt`, replace the current `if(APPLE)` native block with:

```cmake
function(olr_add_native_srt_e2e_tests native_label)
    add_test(NAME e2e_native_srt_smoke
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_smoke.sh" "$<TARGET_FILE:record_harness>" 23601)
    add_test(NAME e2e_native_srt_4cam
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_4cam.sh" "$<TARGET_FILE:sync_harness>" 23610)
    add_test(NAME e2e_native_srt_sync
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_sync.sh" "$<TARGET_FILE:sync_harness>" 23620)
    add_test(NAME e2e_native_srt_trim
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_trim.sh" "$<TARGET_FILE:sync_harness>" 23630)
    add_test(NAME e2e_native_srt_connect
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_connect.sh" "$<TARGET_FILE:sync_harness>" 23640)
    set_tests_properties(
        e2e_native_srt_smoke e2e_native_srt_4cam e2e_native_srt_sync
        e2e_native_srt_trim e2e_native_srt_connect
        PROPERTIES
        LABELS "${native_label}"
        TIMEOUT 180
        RUN_SERIAL TRUE
        ENVIRONMENT "OLR_NATIVE_SRT=1")
endfunction()

if(APPLE)
    olr_add_native_srt_e2e_tests("native-apple-ingest")
elseif(WIN32)
    olr_add_native_srt_e2e_tests("native-windows-ingest")
endif()
```

- [ ] **Step 2: Verify Apple test names stay unchanged**

Run:

```bash
cmake -S . -B build/windows-plan -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos
ctest --test-dir build/windows-plan -N -L native-apple-ingest
```

Expected: output lists exactly these five tests:

```text
e2e_native_srt_smoke
e2e_native_srt_4cam
e2e_native_srt_sync
e2e_native_srt_trim
e2e_native_srt_connect
```

- [ ] **Step 3: Document shared native labels**

In `tests/e2e/SRT_README.md`, update the native section to state:

```markdown
The native SRT e2e scripts are shared across platforms. CTest registers the
same `e2e_native_srt_*` test names for each native platform and changes only the
label:

- macOS/iOS host builds: `native-apple-ingest`
- Windows host builds: `native-windows-ingest`

Both labels run with `OLR_NATIVE_SRT=1`. The producer side still uses local
`ffmpeg` and `srt-live-transmit`; the engine side must not require FFmpeg SRT
support for native ingest.
```

- [ ] **Step 4: Commit**

```bash
git add tests/e2e/CMakeLists.txt tests/e2e/SRT_README.md
git commit -m "test(srt): share native ingest e2e registrations"
```

---

### Task 4: Add Native SRT Address Helper

**Files:**
- Create: `recorder_engine/ingest/nativesrtaddress.h`
- Create: `recorder_engine/ingest/nativesrtaddress.cpp`
- Create: `tests/unit/tst_nativesrtaddress.cpp`
- Modify: `recorder_engine/ingest/nativesrtingestsession.cpp`
- Modify: `tests/unit/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing address tests**

Create `tests/unit/tst_nativesrtaddress.cpp`:

```cpp
#include <QtTest>

#include "recorder_engine/ingest/nativesrtaddress.h"

class TestNativeSrtAddress : public QObject {
    Q_OBJECT
private slots:
    void acceptsNumericIpv4();
    void rejectsNonIpv4Hosts();
    void fillsIpv4Sockaddr();
};

void TestNativeSrtAddress::acceptsNumericIpv4() {
    QVERIFY(nativeSrtIsNumericIpv4Host(QStringLiteral("127.0.0.1")));
    QVERIFY(nativeSrtIsNumericIpv4Host(QStringLiteral("10.20.30.40")));
}

void TestNativeSrtAddress::rejectsNonIpv4Hosts() {
    QVERIFY(!nativeSrtIsNumericIpv4Host(QString()));
    QVERIFY(!nativeSrtIsNumericIpv4Host(QStringLiteral("localhost")));
    QVERIFY(!nativeSrtIsNumericIpv4Host(QStringLiteral("::1")));
    QVERIFY(!nativeSrtIsNumericIpv4Host(QStringLiteral("999.1.1.1")));
}

void TestNativeSrtAddress::fillsIpv4Sockaddr() {
    NativeSrtSockaddr address;
    QVERIFY(nativeSrtMakeIpv4Sockaddr(QStringLiteral("127.0.0.1"), 9000, &address));
    QVERIFY(address.size > 0);
    QVERIFY(address.sockaddrPtr());
}

QTEST_GUILESS_MAIN(TestNativeSrtAddress)
#include "tst_nativesrtaddress.moc"
```

- [ ] **Step 2: Register the test and verify failure**

In `tests/unit/CMakeLists.txt`, add:

```cmake
olr_add_unit_test(tst_nativesrtaddress olr_test_core)
```

Run:

```bash
cmake --build build/windows-plan --target tst_nativesrtaddress -j8
```

Expected: compile failure because `nativesrtaddress.h` does not exist.

- [ ] **Step 3: Implement the helper**

Create `recorder_engine/ingest/nativesrtaddress.h`:

```cpp
#ifndef NATIVESRTADDRESS_H
#define NATIVESRTADDRESS_H

#include <QString>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

struct NativeSrtSockaddr {
    sockaddr_storage storage {};
    int size = 0;

    sockaddr* sockaddrPtr() { return reinterpret_cast<sockaddr*>(&storage); }
    const sockaddr* sockaddrPtr() const { return reinterpret_cast<const sockaddr*>(&storage); }
};

bool nativeSrtIsNumericIpv4Host(const QString& host);
bool nativeSrtMakeIpv4Sockaddr(const QString& host, quint16 port, NativeSrtSockaddr* address);

#endif // NATIVESRTADDRESS_H
```

Create `recorder_engine/ingest/nativesrtaddress.cpp`:

```cpp
#include "nativesrtaddress.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#include <cstring>

bool nativeSrtIsNumericIpv4Host(const QString& host) {
    if (host.isEmpty()) {
        return false;
    }

    sockaddr_in address {};
    return inet_pton(AF_INET, host.toUtf8().constData(), &address.sin_addr) == 1;
}

bool nativeSrtMakeIpv4Sockaddr(const QString& host, quint16 port, NativeSrtSockaddr* address) {
    if (!address || host.isEmpty()) {
        return false;
    }

    sockaddr_in ipv4 {};
    ipv4.sin_family = AF_INET;
    ipv4.sin_port = htons(port);
    if (inet_pton(AF_INET, host.toUtf8().constData(), &ipv4.sin_addr) != 1) {
        return false;
    }

    address->storage = {};
    memcpy(&address->storage, &ipv4, sizeof(ipv4));
    address->size = int(sizeof(ipv4));
    return true;
}
```

- [ ] **Step 4: Use helper in native SRT session**

In `recorder_engine/ingest/nativesrtingestsession.cpp`:

```cpp
#include "nativesrtaddress.h"
```

Remove the local `isNumericIpv4Host()` helper and replace its call:

```cpp
return nativeSrtIsNumericIpv4Host(url.host());
```

In `openSocket()`, replace direct `sockaddr_in` construction:

```cpp
NativeSrtSockaddr address;
if (!nativeSrtMakeIpv4Sockaddr(m_url.host(), quint16(m_url.port(9000)), &address)) {
    if (error) {
        *error = QStringLiteral("Native SRT currently requires a numeric IPv4 host.");
    }
    closeSocket();
    return false;
}

const int connectResult = srt_connect(m_socket, address.sockaddrPtr(), address.size);
```

Then remove `<arpa/inet.h>`, `<netinet/in.h>`, and `<sys/socket.h>` from
`nativesrtingestsession.cpp`; the platform socket headers now live in
`nativesrtaddress.h/.cpp`.

- [ ] **Step 5: Add helper sources to CMake**

Add `recorder_engine/ingest/nativesrtaddress.h` and `.cpp` to app sources and `olr_test_core`.

- [ ] **Step 6: Verify**

Run:

```bash
cmake -S . -B build/windows-plan -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos
cmake --build build/windows-plan --target tst_nativesrtaddress -j8
ctest --test-dir build/windows-plan -R tst_nativesrtaddress --output-on-failure
```

Expected: test passes.

- [ ] **Step 7: Commit**

```bash
git add recorder_engine/ingest/nativesrtaddress.h recorder_engine/ingest/nativesrtaddress.cpp recorder_engine/ingest/nativesrtingestsession.cpp CMakeLists.txt tests/CMakeLists.txt tests/unit/CMakeLists.txt tests/unit/tst_nativesrtaddress.cpp
git commit -m "refactor(srt): isolate native IPv4 address parsing"
```

---

### Task 5: Add CPU Frame Copy Helpers

**Files:**
- Create: `recorder_engine/ingest/nativeframecopy.h`
- Create: `recorder_engine/ingest/nativeframecopy.cpp`
- Create: `tests/unit/tst_nativeframecopy.cpp`
- Modify: `tests/unit/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write failing NV12 copy test**

Create `tests/unit/tst_nativeframecopy.cpp`:

```cpp
#include <QtTest>

#include "recorder_engine/ingest/nativeframecopy.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

class TestNativeFrameCopy : public QObject {
    Q_OBJECT
private slots:
    void nv12CopiesToYuv420pWithStride();
};

void TestNativeFrameCopy::nv12CopiesToYuv420pWithStride() {
    const int width = 4;
    const int height = 4;
    const int yStride = 6;
    const int uvStride = 6;

    QByteArray y(yStride * height, char(0));
    QByteArray uv(uvStride * (height / 2), char(0));

    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            y[row * yStride + col] = char(10 + row * width + col);
        }
    }
    for (int row = 0; row < height / 2; ++row) {
        for (int col = 0; col < width / 2; ++col) {
            uv[row * uvStride + col * 2] = char(80 + row * 2 + col);
            uv[row * uvStride + col * 2 + 1] = char(120 + row * 2 + col);
        }
    }

    AVFrame* frame = nativeCopyNv12ToYuv420p(
        reinterpret_cast<const uint8_t*>(y.constData()), yStride,
        reinterpret_cast<const uint8_t*>(uv.constData()), uvStride,
        width, height);

    QVERIFY(frame);
    QCOMPARE(frame->format, int(AV_PIX_FMT_YUV420P));
    QCOMPARE(frame->width, width);
    QCOMPARE(frame->height, height);
    QCOMPARE(int(frame->data[0][0]), 10);
    QCOMPARE(int(frame->data[0][3]), 13);
    QCOMPARE(int(frame->data[0][frame->linesize[0] + 0]), 14);
    QCOMPARE(int(frame->data[1][0]), 80);
    QCOMPARE(int(frame->data[1][1]), 81);
    QCOMPARE(int(frame->data[2][0]), 120);
    QCOMPARE(int(frame->data[2][1]), 121);

    av_frame_free(&frame);
}

QTEST_GUILESS_MAIN(TestNativeFrameCopy)
#include "tst_nativeframecopy.moc"
```

- [ ] **Step 2: Register and verify failure**

In `tests/unit/CMakeLists.txt`, add:

```cmake
olr_add_unit_test(tst_nativeframecopy olr_test_core)
```

Run:

```bash
cmake --build build/windows-plan --target tst_nativeframecopy -j8
```

Expected: compile failure because `nativeframecopy.h` does not exist.

- [ ] **Step 3: Implement NV12 helper**

Create `recorder_engine/ingest/nativeframecopy.h`:

```cpp
#ifndef NATIVEFRAMECOPY_H
#define NATIVEFRAMECOPY_H

#include <cstdint>

extern "C" {
struct AVFrame;
}

AVFrame* nativeCopyNv12ToYuv420p(const uint8_t* yPlane, int yStride,
                                 const uint8_t* uvPlane, int uvStride,
                                 int width, int height);

#endif // NATIVEFRAMECOPY_H
```

Create `recorder_engine/ingest/nativeframecopy.cpp`:

```cpp
#include "nativeframecopy.h"

#include <cstring>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace {

void copyRows(const uint8_t* src, int srcStride, uint8_t* dst, int dstStride,
              int bytesPerRow, int rows) {
    for (int y = 0; y < rows; ++y) {
        memcpy(dst + y * dstStride, src + y * srcStride, size_t(bytesPerRow));
    }
}

} // namespace

AVFrame* nativeCopyNv12ToYuv420p(const uint8_t* yPlane, int yStride,
                                 const uint8_t* uvPlane, int uvStride,
                                 int width, int height) {
    if (!yPlane || !uvPlane || width <= 0 || height <= 0 || (width % 2) != 0 || (height % 2) != 0) {
        return nullptr;
    }

    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        return nullptr;
    }
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = width;
    frame->height = height;
    if (av_frame_get_buffer(frame, 32) < 0) {
        av_frame_free(&frame);
        return nullptr;
    }

    copyRows(yPlane, yStride, frame->data[0], frame->linesize[0], width, height);
    for (int row = 0; row < height / 2; ++row) {
        const uint8_t* src = uvPlane + row * uvStride;
        uint8_t* u = frame->data[1] + row * frame->linesize[1];
        uint8_t* v = frame->data[2] + row * frame->linesize[2];
        for (int col = 0; col < width / 2; ++col) {
            u[col] = src[col * 2];
            v[col] = src[col * 2 + 1];
        }
    }
    return frame;
}
```

- [ ] **Step 4: Wire CMake and verify**

Add `nativeframecopy.cpp` to app sources and `olr_test_core`, then run:

```bash
cmake -S . -B build/windows-plan -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos
cmake --build build/windows-plan --target tst_nativeframecopy -j8
ctest --test-dir build/windows-plan -R tst_nativeframecopy --output-on-failure
```

Expected: test passes.

- [ ] **Step 5: Commit**

```bash
git add recorder_engine/ingest/nativeframecopy.h recorder_engine/ingest/nativeframecopy.cpp tests/unit/tst_nativeframecopy.cpp tests/unit/CMakeLists.txt tests/CMakeLists.txt CMakeLists.txt
git commit -m "test(ingest): add native NV12 frame copy helper"
```

---

### Task 6: Add Native Decode Fallback Reason

**Files:**
- Modify: `recorder_engine/ingest/ingestsession.h`
- Modify: `recorder_engine/ingest/nativesrtingestsession.h`
- Modify: `recorder_engine/ingest/nativesrtingestsession.cpp`
- Modify: `recorder_engine/streamworker.cpp`
- Modify: `tests/unit/tst_ingestbackendselector.cpp`

- [ ] **Step 1: Add a failing selector/fallback policy test**

Append to `tests/unit/tst_ingestbackendselector.cpp`:

```cpp
void nativeFailureReasonStartsEmpty();
```

Add to the class declaration and implement:

```cpp
class EmptySession final : public IngestSession {
public:
    bool open(const QUrl&, const IngestCallbacks&) override { return false; }
    void run() override {}
    void requestStop() override {}
};

void TestIngestBackendSelector::nativeFailureReasonStartsEmpty() {
    EmptySession session;
    QVERIFY(session.nativeFallbackReason().isEmpty());
}
```

Run:

```bash
cmake --build build/windows-plan --target tst_ingestbackendselector -j8
```

Expected: compile failure because `nativeFallbackReason()` does not exist.

- [ ] **Step 2: Add default fallback API**

In `recorder_engine/ingest/ingestsession.h`, add to `class IngestSession`:

```cpp
virtual QString nativeFallbackReason() const { return QString(); }
```

- [ ] **Step 3: Add NativeSrtIngestSession fallback state**

In `nativesrtingestsession.h`, add public override:

```cpp
QString nativeFallbackReason() const override;
```

Add private state:

```cpp
QString m_nativeFallbackReason;
void markNativeFallback(const QString& reason);
```

In `nativesrtingestsession.cpp`, clear it in `open()`:

```cpp
m_nativeFallbackReason.clear();
```

Add:

```cpp
QString NativeSrtIngestSession::nativeFallbackReason() const {
    return m_nativeFallbackReason;
}

void NativeSrtIngestSession::markNativeFallback(const QString& reason) {
    if (m_nativeFallbackReason.isEmpty()) {
        m_nativeFallbackReason = reason;
    }
    log(reason);
}
```

In video decode failure handling, replace:

```cpp
if (!decoded && !error.isEmpty()) {
    log(error);
}
```

with:

```cpp
if (!decoded && !error.isEmpty()) {
    markNativeFallback(error);
}
```

- [ ] **Step 4: Teach StreamWorker to use FFmpeg after native fallback**

In `recorder_engine/streamworker.cpp`, add a local flag before the source loop creates sessions:

```cpp
bool suppressNativeForCurrentUrl = false;
QString nativeSuppressedUrl;
```

When URL changes, reset:

```cpp
if (nativeSuppressedUrl != currentUrl) {
    nativeSuppressedUrl = currentUrl;
    suppressNativeForCurrentUrl = false;
}
```

When selecting backend:

```cpp
backendOptions.preferNativeSrt = !suppressNativeForCurrentUrl
                                 && qEnvironmentVariableIsSet("OLR_NATIVE_SRT")
                                 && NativeSrtIngestSession::supportsUrl(sourceUrl);
```

After `session->run()` returns, before the next retry:

```cpp
const QString nativeFallbackReason = session->nativeFallbackReason();
if (!nativeFallbackReason.isEmpty()) {
    qDebug() << "Source" << m_sourceIndex
             << "Native ingest fallback requested:" << nativeFallbackReason
             << "Retrying with FFmpeg for this URL.";
    suppressNativeForCurrentUrl = true;
}
```

- [ ] **Step 5: Verify**

Run:

```bash
cmake -S . -B build/windows-plan -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos
cmake --build build/windows-plan --target tst_ingestbackendselector -j8
ctest --test-dir build/windows-plan -R tst_ingestbackendselector --output-on-failure
```

Expected: test passes and existing selector behavior is unchanged.

- [ ] **Step 6: Commit**

```bash
git add recorder_engine/ingest/ingestsession.h recorder_engine/ingest/nativesrtingestsession.h recorder_engine/ingest/nativesrtingestsession.cpp recorder_engine/streamworker.cpp tests/unit/tst_ingestbackendselector.cpp
git commit -m "feat(ingest): fall back after native decode failure"
```

---

### Task 7: Add Windows Media Foundation Capability Probe

**Files:**
- Create: `recorder_engine/ingest/nativevideodecoder_mediafoundation.cpp`
- Modify: `recorder_engine/ingest/nativevideodecoder.h`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Add Windows compile source selection**

In `CMakeLists.txt`, add a Windows platform branch near the Apple native SRT branch:

```cmake
elseif(WIN32)
    target_sources(OpenLiveReplay PRIVATE
        recorder_engine/ingest/nativevideodecoder_mediafoundation.cpp
        recorder_engine/ingest/nativesrtingestsession.cpp)
    target_compile_definitions(OpenLiveReplay PRIVATE OLR_NATIVE_SRT_AVAILABLE=1)
    target_link_libraries(OpenLiveReplay PRIVATE
        mfplat mf mfuuid d3d11 dxgi ole32 ws2_32 srt)
```

In `tests/CMakeLists.txt`, add equivalent `WIN32` sources and libraries for `olr_test_engine`.

- [ ] **Step 2: Implement a probe-only Windows file**

Create `recorder_engine/ingest/nativevideodecoder_mediafoundation.cpp`:

```cpp
#include "nativevideodecoder.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <QStringList>

#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {

QString hrMessage(const QString& action, HRESULT hr) {
    return QStringLiteral("%1 (HRESULT 0x%2)")
        .arg(action)
        .arg(qulonglong(hr), 8, 16, QLatin1Char('0'));
}

bool mftAvailable(REFGUID subtype) {
    MFT_REGISTER_TYPE_INFO input {};
    input.guidMajorType = MFMediaType_Video;
    input.guidSubtype = subtype;

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    const HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_DECODER,
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_LOCALMFT,
        &input,
        nullptr,
        &activates,
        &count);
    if (SUCCEEDED(hr) && activates) {
        for (UINT32 i = 0; i < count; ++i) {
            if (activates[i]) {
                activates[i]->Release();
            }
        }
        CoTaskMemFree(activates);
    }
    return SUCCEEDED(hr) && count > 0;
}

bool d3d11Available(QStringList* detail) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL createdLevel = D3D_FEATURE_LEVEL_10_0;
    const HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        levels,
        UINT(std::size(levels)),
        D3D11_SDK_VERSION,
        &device,
        &createdLevel,
        &context);
    if (FAILED(hr)) {
        if (detail) detail->append(hrMessage(QStringLiteral("D3D11 device creation failed"), hr));
        return false;
    }
    return true;
}

} // namespace

class NativeVideoDecoder::Impl {
public:
    Impl(int, int) {}
};

NativeVideoDecoder::NativeVideoDecoder(int outputWidth, int outputHeight)
    : m_impl(new Impl(outputWidth, outputHeight)) {}

NativeVideoDecoder::~NativeVideoDecoder() {
    delete m_impl;
}

bool NativeVideoDecoder::decode(const CompressedAccessUnit&, FrameCallback, QString* error) {
    if (error) {
        *error = QStringLiteral("Windows native decode is not implemented yet");
    }
    return false;
}

void NativeVideoDecoder::reset() {}

NativeVideoDecodeCapabilities queryNativeVideoDecodeCapabilities() {
    NativeVideoDecodeCapabilities caps;
    QStringList detail;

    const HRESULT startup = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(startup)) {
        caps.detail = hrMessage(QStringLiteral("Media Foundation startup failed"), startup);
        return caps;
    }

    caps.d3d11 = d3d11Available(&detail);
    caps.h264 = mftAvailable(MFVideoFormat_H264);
    caps.hevc = mftAvailable(MFVideoFormat_HEVC);

    if (!caps.h264) detail.append(QStringLiteral("H.264 decoder MFT unavailable"));
    if (!caps.hevc) detail.append(QStringLiteral("HEVC decoder MFT unavailable"));
    if (detail.isEmpty()) detail.append(QStringLiteral("Media Foundation native decode available"));
    caps.detail = detail.join(QStringLiteral("; "));

    MFShutdown();
    return caps;
}

#endif
```

- [ ] **Step 3: Verify on macOS and Windows**

macOS:

```bash
cmake -S . -B build/windows-plan -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos
cmake --build build/windows-plan --target tst_nativevideodecoder -j8
ctest --test-dir build/windows-plan -R tst_nativevideodecoder --output-on-failure
```

Windows:

```powershell
cmake -S . -B build/windows-native -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH="$env:USERPROFILE\Qt\6.10.1\msvc2022_64" -DOLR_SRT_ROOT="C:\deps\srt"
cmake --build build/windows-native --config Debug --target tst_nativevideodecoder
ctest --test-dir build/windows-native -C Debug -R tst_nativevideodecoder --output-on-failure
```

Expected: macOS still passes; Windows test runs and prints no crash. Capability booleans depend on machine configuration.

- [ ] **Step 4: Commit**

```bash
git add recorder_engine/ingest/nativevideodecoder_mediafoundation.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(windows): probe Media Foundation native decode"
```

---

### Task 8: Implement Windows H.264 Decode

**Files:**
- Modify: `recorder_engine/ingest/nativevideodecoder_mediafoundation.cpp`
- Modify: `tests/e2e/CMakeLists.txt`
- Modify: `tests/e2e/SRT_README.md`

- [ ] **Step 1: Verify the native Windows e2e fails before decode exists**

On Windows, configure a build without FFmpeg SRT ingest support and run:

```powershell
cmake -S . -B build/windows-native -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH="$env:USERPROFILE\Qt\6.10.1\msvc2022_64" -DOLR_SRT_ROOT="C:\deps\srt"
cmake --build build/windows-native --config Debug --target record_harness sync_harness
ctest --test-dir build/windows-native -C Debug -L native-windows-ingest --output-on-failure
```

Expected: native H.264 tests fail because `NativeVideoDecoder::decode()` returns "Windows native decode is not implemented yet".

- [ ] **Step 2: Replace the probe-only `Impl` with a Media Foundation decoder**

In `nativevideodecoder_mediafoundation.cpp`, expand `NativeVideoDecoder::Impl` to own:

```cpp
int width = 0;
int height = 0;
NativeVideoCodec codec = NativeVideoCodec::Unknown;
QByteArray activeParameterSetKey;
ComPtr<IMFTransform> transform;
ComPtr<IMFDXGIDeviceManager> deviceManager;
ComPtr<ID3D11Device> d3dDevice;
UINT resetToken = 0;
```

Add methods:

```cpp
bool decode(const CompressedAccessUnit& unit, NativeVideoDecoder::FrameCallback onFrame, QString* error);
void reset();
bool ensureSession(const CompressedAccessUnit& unit, QString* error);
bool createD3D(QString* error);
bool createTransform(NativeVideoCodec codec, QString* error);
bool configureTypes(NativeVideoCodec codec, QString* error);
bool submitSample(const CompressedAccessUnit& unit, QString* error);
bool drain(NativeVideoDecoder::FrameCallback onFrame, qint64 pts90k, QString* error);
```

- [ ] **Step 3: Configure H.264 input and NV12 output**

Use these media subtypes:

```cpp
const GUID inputSubtype = (codec == NativeVideoCodec::H264) ? MFVideoFormat_H264 : MFVideoFormat_HEVC;
const GUID outputSubtype = MFVideoFormat_NV12;
```

Set input type attributes:

```cpp
inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
inputType->SetGUID(MF_MT_SUBTYPE, inputSubtype);
MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, UINT32(width), UINT32(height));
```

Set output type attributes:

```cpp
outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
outputType->SetGUID(MF_MT_SUBTYPE, outputSubtype);
MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, UINT32(width), UINT32(height));
```

Send stream messages after type setup:

```cpp
transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
```

- [ ] **Step 4: Copy output buffers to AVFrame**

For system-memory output, lock the contiguous buffer and call:

```cpp
AVFrame* frame = nativeCopyNv12ToYuv420p(data, width, data + width * height, width, width, height);
```

For DXGI-backed samples, first try `IMF2DBuffer2`:

```cpp
ComPtr<IMF2DBuffer2> buffer2d;
if (SUCCEEDED(buffer.As(&buffer2d))) {
    BYTE* scanline0 = nullptr;
    LONG pitch = 0;
    BYTE* bufferStart = nullptr;
    DWORD bufferLength = 0;
    buffer2d->Lock2DSize(MF2DBuffer_LockFlags_Read, &scanline0, &pitch, &bufferStart, &bufferLength);
    AVFrame* frame = nativeCopyNv12ToYuv420p(scanline0, pitch, scanline0 + pitch * height, pitch, width, height);
    buffer2d->Unlock2D();
}
```

If neither buffer path works, return:

```cpp
*error = QStringLiteral("Media Foundation decoded a frame but output buffer copy failed");
```

- [ ] **Step 5: Verify H.264 parity e2e on Windows**

Run:

```powershell
cmake --build build/windows-native --config Debug --target record_harness sync_harness
ctest --test-dir build/windows-native -C Debug -L native-windows-ingest --output-on-failure
```

Expected: the H.264 shared parity tests pass:

```text
e2e_native_srt_smoke
e2e_native_srt_4cam
e2e_native_srt_sync
e2e_native_srt_trim
e2e_native_srt_connect
```

- [ ] **Step 6: Verify macOS was not regressed**

Run:

```bash
cmake --build build/windows-plan --target tst_ingestbackendselector tst_nativevideodecoder -j8
ctest --test-dir build/windows-plan -R "tst_ingestbackendselector|tst_nativevideodecoder" --output-on-failure
```

- [ ] **Step 7: Commit**

```bash
git add recorder_engine/ingest/nativevideodecoder_mediafoundation.cpp tests/e2e/CMakeLists.txt tests/e2e/SRT_README.md
git commit -m "feat(windows): decode native SRT H264 with Media Foundation"
```

---

### Task 9: Add Windows HEVC Decode and Capability-Gated E2E

**Files:**
- Modify: `recorder_engine/ingest/nativevideodecoder_mediafoundation.cpp`
- Create: `tests/e2e/run_srt_hevc_smoke.sh`
- Modify: `tests/e2e/CMakeLists.txt`
- Modify: `tests/e2e/SRT_README.md`

- [ ] **Step 1: Add HEVC producer script**

Create `tests/e2e/run_srt_hevc_smoke.sh` by copying `run_srt_smoke.sh` and changing only:

```bash
command -v ffmpeg  >/dev/null || { echo "SKIP: ffmpeg not found";  exit 0; }
ffmpeg -hide_banner -encoders 2>/dev/null | grep -Eq '(^| )libx265( |$)|(^| )hevc_' \
    || { echo "SKIP: no HEVC encoder available in ffmpeg"; exit 0; }
```

Use this video encoder selection:

```bash
if ffmpeg -hide_banner -encoders 2>/dev/null | grep -q ' libx265 '; then
    VCODEC_ARGS=(-c:v libx265 -preset ultrafast -x265-params keyint=30:min-keyint=30:scenecut=0 -pix_fmt yuv420p -b:v 4M)
else
    VCODEC_ARGS=(-c:v hevc_videotoolbox -g 30 -pix_fmt yuv420p -b:v 4M)
fi
```

Invoke ffmpeg with:

```bash
"${VCODEC_ARGS[@]}"
```

- [ ] **Step 2: Register HEVC smoke as a separate capability-gated test**

In `tests/e2e/CMakeLists.txt`, under `WIN32`, add:

```cmake
add_test(NAME e2e_native_srt_hevc_smoke
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_srt_hevc_smoke.sh" "$<TARGET_FILE:record_harness>" 23701)
set_tests_properties(e2e_native_srt_hevc_smoke PROPERTIES
    LABELS "native-windows-ingest;native-windows-hevc"
    TIMEOUT 180
    RUN_SERIAL TRUE
    ENVIRONMENT "OLR_NATIVE_SRT=1")
```

- [ ] **Step 3: Ensure HEVC missing falls back clearly**

In `nativevideodecoder_mediafoundation.cpp`, when codec is HEVC and the MFT is unavailable, return:

```cpp
if (error) {
    *error = QStringLiteral("Windows HEVC decoder is unavailable; install Windows HEVC media support or use FFmpeg fallback");
}
return false;
```

This message should become the native fallback reason from Task 6.

- [ ] **Step 4: Verify on Windows with and without HEVC**

With HEVC support:

```powershell
ctest --test-dir build/windows-native -C Debug -R e2e_native_srt_hevc_smoke --output-on-failure
```

Expected: test passes with recorded video and audio.

Without HEVC support:

```powershell
ctest --test-dir build/windows-native -C Debug -R e2e_native_srt_hevc_smoke --output-on-failure
```

Expected: test skips because producer or Windows HEVC capability is absent, or asserts successful fallback depending on the final harness behavior. It must not hang or loop native retries forever.

- [ ] **Step 5: Commit**

```bash
git add recorder_engine/ingest/nativevideodecoder_mediafoundation.cpp tests/e2e/run_srt_hevc_smoke.sh tests/e2e/CMakeLists.txt tests/e2e/SRT_README.md
git commit -m "feat(windows): add native HEVC SRT smoke coverage"
```

---

## Final Verification

- [ ] **macOS unit baseline**

```bash
cmake -S . -B build/windows-plan -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos
cmake --build build/windows-plan --target tst_ingestbackendselector tst_nativevideodecoder tst_nativesrtaddress tst_nativeframecopy -j8
ctest --test-dir build/windows-plan -R "tst_ingestbackendselector|tst_nativevideodecoder|tst_nativesrtaddress|tst_nativeframecopy" --output-on-failure
```

- [ ] **macOS native Apple parity still registered**

```bash
ctest --test-dir build/windows-plan -N -L native-apple-ingest
```

Expected: five shared native SRT tests listed.

- [ ] **Windows H.264 native parity**

```powershell
cmake -S . -B build/windows-native -DOLR_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH="$env:USERPROFILE\Qt\6.10.1\msvc2022_64" -DOLR_SRT_ROOT="C:\deps\srt"
cmake --build build/windows-native --config Debug --target record_harness sync_harness
ctest --test-dir build/windows-native -C Debug -L native-windows-ingest --output-on-failure
```

Expected: H.264 native SRT smoke, 4cam, sync, trim, and connect pass.

- [ ] **Windows HEVC capability**

```powershell
ctest --test-dir build/windows-native -C Debug -R e2e_native_srt_hevc_smoke --output-on-failure
```

Expected: pass on HEVC-capable Windows machines; skip or graceful fallback assertion on machines without HEVC support.

## Spec Coverage Self-Review

- Native decoder boundary: Tasks 1 and 2.
- Windows Media Foundation/D3D11 path: Tasks 7, 8, and 9.
- Native SRT portability: Task 4.
- Runtime capability probing: Tasks 1 and 7.
- FFmpeg fallback after native failure: Task 6.
- Shared e2e parity without duplicated scripts: Task 3.
- H.264 parity coverage: Task 8.
- HEVC capability-gated coverage: Task 9.
- Existing Apple path preservation: Tasks 2, 3, and Final Verification.
