# NDI Output Validation Foundation + Tier (a) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove the real NDI output transport is reliable — a marker stream sent through a real `NdiOutputSink` is captured by an external NDI receiver and checked for dropped/duplicated/reordered frames, A-V desync, and cadence stalls.

**Architecture:** Two pure, unit-tested modules (a pixel-encoded marker codec and the capture-analysis math) plus two runtime-loaded NDI harnesses (a receiver probe and a tier-(a) sender that feeds a real `NdiOutputSink`), wired by a bash driver under an opt-in CTest label that SKIPs when no NDI runtime is present. No production source changes.

**Tech Stack:** C++17, Qt 6 Core (`QByteArray`, `QLibrary`), Qt Test, the NDI ABI/runtime-loading already in `playback/output/ndiabi.h` + `ndiruntimepaths.h`, the real `NdiOutputSink` (in `olr_test_playback`), CMake/CTest, bash.

## Global Constraints

- No production source changes. Only new/modified files under `tests/**` and `.githooks/pre-push`.
- Build/test toolchain: `cmake -S . -B build/claude-debug -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON`; build with `~/Qt/Tools/Ninja/ninja -C build/claude-debug <target>`.
- Format ONLY the specific files you changed (never repo-wide): `/opt/homebrew/opt/llvm/bin/clang-format -i <files>`.
- Opt-in CTest label is exactly `ndi-output` (NOT `ci`, NOT `e2e`). The e2e test uses `SKIP_RETURN_CODE 77`.
- The harnesses load the NDI runtime dynamically via `olr::ndi::runtimeLibraryCandidates()` and the ABI in `playback/output/ndiabi.h`; they must build WITHOUT the NDI SDK headers/libs.
- Video over NDI is I420 (`olr::ndi::kFourCcI420`); audio is FLTp planar float (`kFourCcFltp`); 48000 Hz, stereo.
- Marker dimensions: width 256, height 144 (both even; width ≥ 192 to fit the counter).

---

### Task 1: Marker codec (pixel-encoded counter + flash + audio beep)

**Files:**
- Create: `tests/e2e/ndi_output_marker.h`, `tests/e2e/ndi_output_marker.cpp`
- Create: `tests/unit/tst_ndioutputmarker.cpp`
- Modify: `tests/unit/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `struct NdiOutputMarkerConfig { int width=256; int height=144; int fpsNum=30; int fpsDen=1; int sampleRate=48000; int channels=2; int flashPeriod=15; };`
  - `int ndiMarkerSamplesPerFrame(const NdiOutputMarkerConfig&);`
  - `bool ndiMarkerIsFlashFrame(const NdiOutputMarkerConfig&, qint64 frameIndex);`
  - `QByteArray ndiMarkerLumaPlane(const NdiOutputMarkerConfig&, qint64 frameIndex);` — width*height Y plane.
  - `qint64 ndiMarkerDecodeIndex(const NdiOutputMarkerConfig&, const uchar* luma, int stride);` — −1 if unreadable.
  - `bool ndiMarkerDecodeFlash(const NdiOutputMarkerConfig&, const uchar* luma, int stride);`
  - `QByteArray ndiMarkerAudioS16(const NdiOutputMarkerConfig&, qint64 frameIndex);` — interleaved S16 stereo, `ndiMarkerSamplesPerFrame` frames.
  - `double ndiMarkerAudioRmsFltp(const float* plane, int samples);`

- [ ] **Step 1: Write the failing unit test**

Create `tests/unit/tst_ndioutputmarker.cpp`:

```cpp
#include <QtTest>

#include "tests/e2e/ndi_output_marker.h"

class TestNdiOutputMarker : public QObject {
    Q_OBJECT
private slots:
    void lumaCounterRoundTripsForManyIndices();
    void flashCellSetOnlyOnFlashFrames();
    void audioBeepEnergyOnlyOnFlashFrames();
};

void TestNdiOutputMarker::lumaCounterRoundTripsForManyIndices() {
    NdiOutputMarkerConfig cfg;
    for (qint64 i : {qint64(0), qint64(1), qint64(2), qint64(63), qint64(1000), qint64(65535),
                     qint64(1 << 20)}) {
        const QByteArray y = ndiMarkerLumaPlane(cfg, i);
        QCOMPARE(y.size(), cfg.width * cfg.height);
        const qint64 decoded =
            ndiMarkerDecodeIndex(cfg, reinterpret_cast<const uchar*>(y.constData()), cfg.width);
        QCOMPARE(decoded, i);
    }
}

void TestNdiOutputMarker::flashCellSetOnlyOnFlashFrames() {
    NdiOutputMarkerConfig cfg;
    for (qint64 i = 0; i < 60; ++i) {
        const QByteArray y = ndiMarkerLumaPlane(cfg, i);
        const bool flash =
            ndiMarkerDecodeFlash(cfg, reinterpret_cast<const uchar*>(y.constData()), cfg.width);
        QCOMPARE(flash, ndiMarkerIsFlashFrame(cfg, i));
    }
}

void TestNdiOutputMarker::audioBeepEnergyOnlyOnFlashFrames() {
    NdiOutputMarkerConfig cfg;
    const int n = ndiMarkerSamplesPerFrame(cfg);
    for (qint64 i = 0; i < 45; ++i) {
        const QByteArray pcm = ndiMarkerAudioS16(cfg, i);
        QCOMPARE(pcm.size(), n * cfg.channels * int(sizeof(qint16)));
        // Convert interleaved S16 -> one channel of float to reuse the FLTp RMS helper.
        std::vector<float> ch0(n);
        const auto* s = reinterpret_cast<const qint16*>(pcm.constData());
        for (int k = 0; k < n; ++k) ch0[k] = float(s[k * cfg.channels]) / 32768.0f;
        const double rms = ndiMarkerAudioRmsFltp(ch0.data(), n);
        if (ndiMarkerIsFlashFrame(cfg, i)) {
            QVERIFY2(rms > 0.05, "flash frame must carry an audible beep");
        } else {
            QVERIFY2(rms < 1e-4, "non-flash frame must be silent");
        }
    }
}

QTEST_GUILESS_MAIN(TestNdiOutputMarker)
#include "tst_ndioutputmarker.moc"
```

- [ ] **Step 2: Wire the test (it won't compile yet)**

In `tests/unit/CMakeLists.txt`, after the existing `tst_ndimarkerpattern` registration, add:

```cmake
olr_add_unit_test(tst_ndioutputmarker)
target_sources(tst_ndioutputmarker PRIVATE "${CMAKE_SOURCE_DIR}/tests/e2e/ndi_output_marker.cpp")
target_include_directories(tst_ndioutputmarker PRIVATE "${CMAKE_SOURCE_DIR}")
```

Run: `cmake -S . -B build/claude-debug -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON` then `~/Qt/Tools/Ninja/ninja -C build/claude-debug tst_ndioutputmarker`
Expected: compile FAILS — `ndi_output_marker.h` does not exist.

- [ ] **Step 3: Implement the marker codec**

Create `tests/e2e/ndi_output_marker.h`:

```cpp
#ifndef OLR_NDI_OUTPUT_MARKER_H
#define OLR_NDI_OUTPUT_MARKER_H

#include <QByteArray>
#include <QtGlobal>

// A deterministic test marker designed to survive I420/NDI transport: the luma plane carries
// a block-coded frame counter (top-left) and a flash cell (top-right); marker frames also
// carry an audio tone burst. The receiver reads these back to detect dropped/duplicated/
// reordered frames and to measure audio-video sync.
struct NdiOutputMarkerConfig {
    int width = 256;
    int height = 144;
    int fpsNum = 30;
    int fpsDen = 1;
    int sampleRate = 48000;
    int channels = 2;
    int flashPeriod = 15; // a flash every Nth frame
};

int ndiMarkerSamplesPerFrame(const NdiOutputMarkerConfig& cfg);
bool ndiMarkerIsFlashFrame(const NdiOutputMarkerConfig& cfg, qint64 frameIndex);
QByteArray ndiMarkerLumaPlane(const NdiOutputMarkerConfig& cfg, qint64 frameIndex);
qint64 ndiMarkerDecodeIndex(const NdiOutputMarkerConfig& cfg, const uchar* luma, int stride);
bool ndiMarkerDecodeFlash(const NdiOutputMarkerConfig& cfg, const uchar* luma, int stride);
QByteArray ndiMarkerAudioS16(const NdiOutputMarkerConfig& cfg, qint64 frameIndex);
double ndiMarkerAudioRmsFltp(const float* plane, int samples);

#endif // OLR_NDI_OUTPUT_MARKER_H
```

Create `tests/e2e/ndi_output_marker.cpp`:

```cpp
#include "tests/e2e/ndi_output_marker.h"

#include <cmath>

namespace {
constexpr int kCell = 8;       // cell size in pixels
constexpr int kCounterBits = 24;
constexpr uchar kHi = 235;     // "1" / bright
constexpr uchar kLo = 16;      // "0" / dark
constexpr uchar kBg = 128;     // neutral background
constexpr double kBeepHz = 1000.0;
constexpr double kBeepAmp = 0.4;

// Center pixel of cell (col,row) read with clamping to the plane.
uchar cellSample(const uchar* luma, int stride, int width, int height, int col, int row) {
    const int x = qMin(width - 1, col * kCell + kCell / 2);
    const int y = qMin(height - 1, row * kCell + kCell / 2);
    return luma[y * stride + x];
}

void fillCell(QByteArray& y, int width, int col, int row, uchar value) {
    auto* p = reinterpret_cast<uchar*>(y.data());
    for (int dy = 0; dy < kCell; ++dy) {
        for (int dx = 0; dx < kCell; ++dx) {
            p[(row * kCell + dy) * width + (col * kCell + dx)] = value;
        }
    }
}
} // namespace

int ndiMarkerSamplesPerFrame(const NdiOutputMarkerConfig& cfg) {
    if (cfg.fpsNum <= 0) return cfg.sampleRate / 30;
    return int((qint64(cfg.sampleRate) * cfg.fpsDen) / cfg.fpsNum);
}

bool ndiMarkerIsFlashFrame(const NdiOutputMarkerConfig& cfg, qint64 frameIndex) {
    const int period = qMax(1, cfg.flashPeriod);
    return (frameIndex % period) == 0;
}

QByteArray ndiMarkerLumaPlane(const NdiOutputMarkerConfig& cfg, qint64 frameIndex) {
    QByteArray y(cfg.width * cfg.height, char(kBg));
    // Counter: bit i in cell column i of the top row, MSB at column 0.
    const quint32 idx = quint32(frameIndex) & ((1u << kCounterBits) - 1u);
    for (int bit = 0; bit < kCounterBits; ++bit) {
        const bool one = (idx >> (kCounterBits - 1 - bit)) & 1u;
        fillCell(y, cfg.width, bit, 0, one ? kHi : kLo);
    }
    // Flash cell: last cell column of the SECOND row (away from the counter row).
    const int flashCol = (cfg.width / kCell) - 1;
    fillCell(y, cfg.width, flashCol, 1, ndiMarkerIsFlashFrame(cfg, frameIndex) ? kHi : kLo);
    return y;
}

qint64 ndiMarkerDecodeIndex(const NdiOutputMarkerConfig& cfg, const uchar* luma, int stride) {
    if (!luma || stride < cfg.width) return -1;
    quint32 idx = 0;
    for (int bit = 0; bit < kCounterBits; ++bit) {
        const uchar v = cellSample(luma, stride, cfg.width, cfg.height, bit, 0);
        idx = (idx << 1) | (v > 128 ? 1u : 0u);
    }
    return qint64(idx);
}

bool ndiMarkerDecodeFlash(const NdiOutputMarkerConfig& cfg, const uchar* luma, int stride) {
    if (!luma || stride < cfg.width) return false;
    const int flashCol = (cfg.width / kCell) - 1;
    return cellSample(luma, stride, cfg.width, cfg.height, flashCol, 1) > 128;
}

QByteArray ndiMarkerAudioS16(const NdiOutputMarkerConfig& cfg, qint64 frameIndex) {
    const int n = ndiMarkerSamplesPerFrame(cfg);
    QByteArray pcm(n * cfg.channels * int(sizeof(qint16)), '\0');
    if (!ndiMarkerIsFlashFrame(cfg, frameIndex)) return pcm;
    auto* s = reinterpret_cast<qint16*>(pcm.data());
    for (int k = 0; k < n; ++k) {
        const double t = double(k) / double(cfg.sampleRate);
        const double v = kBeepAmp * std::sin(2.0 * M_PI * kBeepHz * t);
        const qint16 sample = qint16(qBound(-1.0, v, 1.0) * 32767.0);
        for (int ch = 0; ch < cfg.channels; ++ch) s[k * cfg.channels + ch] = sample;
    }
    return pcm;
}

double ndiMarkerAudioRmsFltp(const float* plane, int samples) {
    if (!plane || samples <= 0) return 0.0;
    double sum = 0.0;
    for (int i = 0; i < samples; ++i) sum += double(plane[i]) * double(plane[i]);
    return std::sqrt(sum / double(samples));
}
```

- [ ] **Step 4: Build and run the test**

Run: `~/Qt/Tools/Ninja/ninja -C build/claude-debug tst_ndioutputmarker && ./build/claude-debug/tests/unit/tst_ndioutputmarker`
Expected: `Totals: 3 passed, 0 failed`.

- [ ] **Step 5: Format and commit**

```bash
/opt/homebrew/opt/llvm/bin/clang-format -i tests/e2e/ndi_output_marker.h tests/e2e/ndi_output_marker.cpp tests/unit/tst_ndioutputmarker.cpp
git add tests/e2e/ndi_output_marker.h tests/e2e/ndi_output_marker.cpp tests/unit/tst_ndioutputmarker.cpp tests/unit/CMakeLists.txt
git commit -m "test: add NDI output marker codec (pixel counter + flash + beep)"
```

---

### Task 2: Capture-analysis math (continuity, A-V sync, cadence)

**Files:**
- Create: `tests/e2e/ndi_recv_analysis.h`, `tests/e2e/ndi_recv_analysis.cpp`
- Create: `tests/unit/tst_ndirecvanalysis.cpp`
- Modify: `tests/unit/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `struct NdiContinuity { qint64 framesReceived; qint64 drops; qint64 dupes; qint64 reorders; };`
  - `NdiContinuity ndiAnalyzeContinuity(const std::vector<qint64>& decodedIndices);`
  - `int ndiAvSyncMaxFrames(const std::vector<qint64>& videoFlashIndices, const std::vector<qint64>& audioBeepFrameIndices);` — −1 if either is empty.
  - `struct NdiCadence { int maxGapFrames; double meanRateHz; };`
  - `NdiCadence ndiAnalyzeCadence(const std::vector<double>& arrivalSeconds, int fpsNum, int fpsDen);`

- [ ] **Step 1: Write the failing unit test**

Create `tests/unit/tst_ndirecvanalysis.cpp`:

```cpp
#include <QtTest>

#include "tests/e2e/ndi_recv_analysis.h"

class TestNdiRecvAnalysis : public QObject {
    Q_OBJECT
private slots:
    void continuityCountsDropsDupesReorders();
    void avSyncIsMaxNearestFlashBeepOffset();
    void cadenceReportsMaxGapAndMeanRate();
};

void TestNdiRecvAnalysis::continuityCountsDropsDupesReorders() {
    // perfect run: 0..5
    auto perfect = ndiAnalyzeContinuity({0, 1, 2, 3, 4, 5});
    QCOMPARE(perfect.framesReceived, qint64(6));
    QCOMPARE(perfect.drops, qint64(0));
    QCOMPARE(perfect.dupes, qint64(0));
    QCOMPARE(perfect.reorders, qint64(0));

    // one drop (2 missing): 0,1,3,4
    auto dropped = ndiAnalyzeContinuity({0, 1, 3, 4});
    QCOMPARE(dropped.drops, qint64(1));

    // a duplicate frame: 0,1,1,2
    auto duped = ndiAnalyzeContinuity({0, 1, 1, 2});
    QCOMPARE(duped.dupes, qint64(1));

    // a reorder: 0,2,1,3
    auto reordered = ndiAnalyzeContinuity({0, 2, 1, 3});
    QCOMPARE(reordered.reorders, qint64(1));
}

void TestNdiRecvAnalysis::avSyncIsMaxNearestFlashBeepOffset() {
    // flashes at 0,15,30 ; beeps at 0,16,30 -> offsets 0,1,0 -> max 1
    QCOMPARE(ndiAvSyncMaxFrames({0, 15, 30}, {0, 16, 30}), 1);
    QCOMPARE(ndiAvSyncMaxFrames({0, 15, 30}, {0, 15, 30}), 0);
    QCOMPARE(ndiAvSyncMaxFrames({}, {0}), -1);
}

void TestNdiRecvAnalysis::cadenceReportsMaxGapAndMeanRate() {
    // 30fps nominal; arrivals every ~33.33ms, one double-gap in the middle
    std::vector<double> arr;
    double t = 0.0;
    for (int i = 0; i < 10; ++i) {
        arr.push_back(t);
        t += (i == 4) ? (2.0 / 30.0) : (1.0 / 30.0); // one 2x gap
    }
    const NdiCadence c = ndiAnalyzeCadence(arr, 30, 1);
    QCOMPARE(c.maxGapFrames, 2);
    QVERIFY(std::abs(c.meanRateHz - 30.0) < 3.0);
}

QTEST_GUILESS_MAIN(TestNdiRecvAnalysis)
#include "tst_ndirecvanalysis.moc"
```

- [ ] **Step 2: Wire the test (won't compile yet)**

In `tests/unit/CMakeLists.txt`, after the `tst_ndioutputmarker` block, add:

```cmake
olr_add_unit_test(tst_ndirecvanalysis)
target_sources(tst_ndirecvanalysis PRIVATE "${CMAKE_SOURCE_DIR}/tests/e2e/ndi_recv_analysis.cpp")
target_include_directories(tst_ndirecvanalysis PRIVATE "${CMAKE_SOURCE_DIR}")
```

Run: `cmake -S . -B build/claude-debug -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON` then `~/Qt/Tools/Ninja/ninja -C build/claude-debug tst_ndirecvanalysis`
Expected: compile FAILS — `ndi_recv_analysis.h` does not exist.

- [ ] **Step 3: Implement the analysis**

Create `tests/e2e/ndi_recv_analysis.h`:

```cpp
#ifndef OLR_NDI_RECV_ANALYSIS_H
#define OLR_NDI_RECV_ANALYSIS_H

#include <QtGlobal>

#include <vector>

struct NdiContinuity {
    qint64 framesReceived = 0;
    qint64 drops = 0;
    qint64 dupes = 0;
    qint64 reorders = 0;
};

// Counts, over the decoded frame-index sequence: missing indices (drops), repeated indices
// (dupes), and out-of-order steps (reorders). A step from a to b: b==a -> dupe; b<a -> reorder;
// b>a+1 -> (b-a-1) drops; b==a+1 -> clean.
NdiContinuity ndiAnalyzeContinuity(const std::vector<qint64>& decodedIndices);

// Max over each video flash of the distance to its nearest audio-beep frame index. −1 if
// either list is empty.
int ndiAvSyncMaxFrames(const std::vector<qint64>& videoFlashIndices,
                       const std::vector<qint64>& audioBeepFrameIndices);

struct NdiCadence {
    int maxGapFrames = 0;
    double meanRateHz = 0.0;
};

// From video arrival timestamps (seconds), the largest inter-arrival gap expressed in frame
// periods (rounded) and the mean delivery rate. Fewer than two arrivals -> {0, 0}.
NdiCadence ndiAnalyzeCadence(const std::vector<double>& arrivalSeconds, int fpsNum, int fpsDen);

#endif // OLR_NDI_RECV_ANALYSIS_H
```

Create `tests/e2e/ndi_recv_analysis.cpp`:

```cpp
#include "tests/e2e/ndi_recv_analysis.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

NdiContinuity ndiAnalyzeContinuity(const std::vector<qint64>& decodedIndices) {
    NdiContinuity out;
    out.framesReceived = qint64(decodedIndices.size());
    for (size_t i = 1; i < decodedIndices.size(); ++i) {
        const qint64 a = decodedIndices[i - 1];
        const qint64 b = decodedIndices[i];
        if (b == a) {
            out.dupes++;
        } else if (b < a) {
            out.reorders++;
        } else if (b > a + 1) {
            out.drops += (b - a - 1);
        }
    }
    return out;
}

int ndiAvSyncMaxFrames(const std::vector<qint64>& videoFlashIndices,
                       const std::vector<qint64>& audioBeepFrameIndices) {
    if (videoFlashIndices.empty() || audioBeepFrameIndices.empty()) return -1;
    int worst = 0;
    for (const qint64 v : videoFlashIndices) {
        qint64 best = std::numeric_limits<qint64>::max();
        for (const qint64 b : audioBeepFrameIndices) best = std::min(best, std::llabs(v - b));
        worst = std::max(worst, int(best));
    }
    return worst;
}

NdiCadence ndiAnalyzeCadence(const std::vector<double>& arrivalSeconds, int fpsNum, int fpsDen) {
    NdiCadence out;
    if (arrivalSeconds.size() < 2 || fpsNum <= 0) return out;
    const double period = double(fpsDen) / double(fpsNum);
    double maxGap = 0.0;
    for (size_t i = 1; i < arrivalSeconds.size(); ++i)
        maxGap = std::max(maxGap, arrivalSeconds[i] - arrivalSeconds[i - 1]);
    out.maxGapFrames = int(std::lround(maxGap / period));
    const double span = arrivalSeconds.back() - arrivalSeconds.front();
    out.meanRateHz = span > 0.0 ? double(arrivalSeconds.size() - 1) / span : 0.0;
    return out;
}
```

- [ ] **Step 4: Build and run the test**

Run: `~/Qt/Tools/Ninja/ninja -C build/claude-debug tst_ndirecvanalysis && ./build/claude-debug/tests/unit/tst_ndirecvanalysis`
Expected: `Totals: 3 passed, 0 failed`.

- [ ] **Step 5: Format and commit**

```bash
/opt/homebrew/opt/llvm/bin/clang-format -i tests/e2e/ndi_recv_analysis.h tests/e2e/ndi_recv_analysis.cpp tests/unit/tst_ndirecvanalysis.cpp
git add tests/e2e/ndi_recv_analysis.h tests/e2e/ndi_recv_analysis.cpp tests/unit/tst_ndirecvanalysis.cpp tests/unit/CMakeLists.txt
git commit -m "test: add NDI receive analysis (continuity, A-V sync, cadence)"
```

---

### Task 3: NDI receiver probe harness

**Files:**
- Create: `tests/e2e/ndi_recv_probe.cpp`
- Modify: `tests/e2e/CMakeLists.txt` (target only; the test registration is Task 5)

**Interfaces:**
- Consumes: `olr::ndi` ABI (`playback/output/ndiabi.h`), `olr::ndi::runtimeLibraryCandidates()` (`ndiruntimepaths.h`), the Task 1 marker decode (`ndiMarkerDecodeIndex`, `ndiMarkerDecodeFlash`, `ndiMarkerAudioRmsFltp`, `NdiOutputMarkerConfig`, `ndiMarkerSamplesPerFrame`), the Task 2 analysis.
- Produces: the `ndi_recv_probe` executable that prints one `NDIRECV ...` line and exits 0 (or 77 to skip / 1 on capture error).

- [ ] **Step 1: Write the probe**

Create `tests/e2e/ndi_recv_probe.cpp`:

```cpp
// Headless NDI receiver probe: discovers an OLR NDI source, captures video+audio for a
// bounded window, decodes the marker, and prints continuity / A-V sync / cadence metrics.
// Runtime-loaded (no NDI SDK at build time). Exits 77 (SKIP) if the runtime is absent or no
// source appears; 1 on a hard capture error; 0 otherwise (the driver decides pass/fail).
//
// usage: ndi_recv_probe <source-name-substring> <capture-seconds>
// env: OLR_NDI_RUNTIME_LIBRARY (override), OLR_NDI_FIND_TIMEOUT_MS (default 5000)
#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLibrary>
#include <QString>

#include <cstdio>
#include <vector>

#include "playback/output/ndiabi.h"
#include "playback/output/ndiruntimepaths.h"
#include "tests/e2e/ndi_output_marker.h"
#include "tests/e2e/ndi_recv_analysis.h"

using namespace olr::ndi;

namespace {
constexpr int kSkip = 77;

struct Recv {
    QLibrary lib;
    NDIlib_initialize_fn init = nullptr;
    NDIlib_destroy_fn destroy = nullptr;
    NDIlib_find_create_v2_fn findCreate = nullptr;
    NDIlib_find_destroy_fn findDestroy = nullptr;
    NDIlib_find_wait_for_sources_fn findWait = nullptr;
    NDIlib_find_get_current_sources_fn findSources = nullptr;
    NDIlib_recv_create_v3_fn recvCreate = nullptr;
    NDIlib_recv_destroy_fn recvDestroy = nullptr;
    NDIlib_recv_capture_v3_fn recvCapture = nullptr;
    NDIlib_recv_free_video_v2_fn freeVideo = nullptr;
    NDIlib_recv_free_audio_v3_fn freeAudio = nullptr;

    bool load() {
        for (const QString& candidate : runtimeLibraryCandidates()) {
            if (candidate.isEmpty()) continue;
            lib.setFileName(candidate);
            if (!lib.load()) continue;
            init = reinterpret_cast<NDIlib_initialize_fn>(lib.resolve("NDIlib_initialize"));
            destroy = reinterpret_cast<NDIlib_destroy_fn>(lib.resolve("NDIlib_destroy"));
            findCreate = reinterpret_cast<NDIlib_find_create_v2_fn>(
                lib.resolve("NDIlib_find_create_v2"));
            findDestroy =
                reinterpret_cast<NDIlib_find_destroy_fn>(lib.resolve("NDIlib_find_destroy"));
            findWait = reinterpret_cast<NDIlib_find_wait_for_sources_fn>(
                lib.resolve("NDIlib_find_wait_for_sources"));
            findSources = reinterpret_cast<NDIlib_find_get_current_sources_fn>(
                lib.resolve("NDIlib_find_get_current_sources"));
            recvCreate = reinterpret_cast<NDIlib_recv_create_v3_fn>(
                lib.resolve("NDIlib_recv_create_v3"));
            recvDestroy =
                reinterpret_cast<NDIlib_recv_destroy_fn>(lib.resolve("NDIlib_recv_destroy"));
            recvCapture = reinterpret_cast<NDIlib_recv_capture_v3_fn>(
                lib.resolve("NDIlib_recv_capture_v3"));
            freeVideo = reinterpret_cast<NDIlib_recv_free_video_v2_fn>(
                lib.resolve("NDIlib_recv_free_video_v2"));
            freeAudio = reinterpret_cast<NDIlib_recv_free_audio_v3_fn>(
                lib.resolve("NDIlib_recv_free_audio_v3"));
            if (findCreate && findSources && recvCreate && recvCapture && freeVideo && freeAudio) {
                if (init) init();
                return true;
            }
            lib.unload();
        }
        return false;
    }
};
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 3) {
        fprintf(stderr, "usage: ndi_recv_probe <source-substring> <capture-seconds>\n");
        return 2;
    }
    const QString want = QString::fromUtf8(argv[1]);
    const double seconds = QString::fromUtf8(argv[2]).toDouble();
    const int findTimeoutMs = qEnvironmentVariableIntValue("OLR_NDI_FIND_TIMEOUT_MS") > 0
                                  ? qEnvironmentVariableIntValue("OLR_NDI_FIND_TIMEOUT_MS")
                                  : 5000;

    Recv ndi;
    if (!ndi.load()) {
        fprintf(stderr, "[ndi_recv_probe] NDI runtime not available - SKIP\n");
        return kSkip;
    }

    NDIlib_find_create_t findCfg;
    NDIlib_find_instance_t finder = ndi.findCreate(&findCfg);
    if (!finder) {
        fprintf(stderr, "[ndi_recv_probe] find_create failed - SKIP\n");
        return kSkip;
    }

    NDIlib_source_t chosen;
    bool found = false;
    QElapsedTimer findTimer;
    findTimer.start();
    while (findTimer.elapsed() < findTimeoutMs && !found) {
        if (ndi.findWait) ndi.findWait(finder, 1000);
        quint32 count = 0;
        const NDIlib_source_t* sources = ndi.findSources(finder, &count);
        for (quint32 i = 0; i < count; ++i) {
            const QString name = QString::fromUtf8(sources[i].p_ndi_name ? sources[i].p_ndi_name
                                                                         : "");
            if (name.contains(want)) {
                chosen = sources[i];
                // Copy the name string into a stable buffer (the source array is owned by NDI).
                static QByteArray nameBuf;
                nameBuf = name.toUtf8();
                chosen.p_ndi_name = nameBuf.constData();
                chosen.p_url_address = nullptr;
                found = true;
                break;
            }
        }
    }
    if (!found) {
        fprintf(stderr, "[ndi_recv_probe] no source matching '%s' - SKIP\n", want.toUtf8().constData());
        if (ndi.findDestroy) ndi.findDestroy(finder);
        return kSkip;
    }

    NDIlib_recv_create_v3_t recvCfg;
    recvCfg.source_to_connect_to = chosen;
    recvCfg.color_format = 3; // fastest / native (I420 from our sender)
    recvCfg.bandwidth = 100;  // highest
    recvCfg.p_ndi_recv_name = "olr-ndi-recv-probe";
    NDIlib_recv_instance_t recv = ndi.recvCreate(&recvCfg);
    if (ndi.findDestroy) ndi.findDestroy(finder);
    if (!recv) {
        fprintf(stderr, "[ndi_recv_probe] recv_create failed\n");
        return 1;
    }

    NdiOutputMarkerConfig mk; // must match the sender's config defaults
    std::vector<qint64> indices;
    std::vector<double> arrivals;
    std::vector<qint64> flashes;
    std::vector<qint64> beeps;

    // Map audio to a frame index by counting received audio samples.
    qint64 audioSamplePos = 0;
    const int samplesPerFrame = ndiMarkerSamplesPerFrame(mk);

    QElapsedTimer run;
    run.start();
    while (run.elapsed() < qint64(seconds * 1000.0)) {
        NDIlib_video_frame_v2_t v;
        NDIlib_audio_frame_v3_t a;
        const int type = ndi.recvCapture(recv, &v, &a, nullptr, 200);
        if (type == FrameTypeVideo) {
            if (v.p_data && v.xres >= mk.width && v.yres >= mk.height) {
                const qint64 idx = ndiMarkerDecodeIndex(
                    mk, reinterpret_cast<const uchar*>(v.p_data), v.line_stride_in_bytes);
                if (idx >= 0) {
                    indices.push_back(idx); // absolute index -> continuity (drops/dupes/reorders)
                    arrivals.push_back(run.elapsed() / 1000.0);
                    if (ndiMarkerDecodeFlash(mk, reinterpret_cast<const uchar*>(v.p_data),
                                             v.line_stride_in_bytes)) {
                        // A-V sync compares capture-RELATIVE positions (the receiver joins
                        // mid-stream, so absolute indices are not comparable to audio, which
                        // carries no index): the video ordinal since capture start vs the
                        // audio-sample-derived ordinal of each beep.
                        flashes.push_back(qint64(indices.size()) - 1);
                    }
                }
            }
            ndi.freeVideo(recv, &v);
        } else if (type == FrameTypeAudio) {
            if (a.p_data && a.no_samples > 0) {
                const double rms =
                    ndiMarkerAudioRmsFltp(reinterpret_cast<const float*>(a.p_data), a.no_samples);
                if (rms > 0.05 && samplesPerFrame > 0)
                    beeps.push_back(audioSamplePos / samplesPerFrame);
                audioSamplePos += a.no_samples;
            }
            ndi.freeAudio(recv, &a);
        }
    }
    ndi.recvDestroy(recv);
    if (ndi.destroy) ndi.destroy();

    const NdiContinuity cont = ndiAnalyzeContinuity(indices);
    const int avSync = ndiAvSyncMaxFrames(flashes, beeps);
    const NdiCadence cad = ndiAnalyzeCadence(arrivals, mk.fpsNum, mk.fpsDen);

    printf("NDIRECV source=%s framesReceived=%lld drops=%lld dupes=%lld reorders=%lld "
           "avSyncMaxFrames=%d maxGapFrames=%d meanRateHz=%.3f\n",
           want.toUtf8().constData(), (long long) cont.framesReceived, (long long) cont.drops,
           (long long) cont.dupes, (long long) cont.reorders, avSync, cad.maxGapFrames,
           cad.meanRateHz);
    fflush(stdout);
    return 0;
}
```

- [ ] **Step 2: Add the target to `tests/e2e/CMakeLists.txt`**

After the `ndi_runtime_sender` target block, add:

```cmake
# NDI receiver probe + tier-(a) sender for the rung-5 output validation lane. Both
# runtime-load the NDI library (no SDK at build time) and link the shared marker/analysis
# modules.
qt_add_executable(ndi_recv_probe
    ndi_recv_probe.cpp
    ndi_output_marker.cpp
    ndi_recv_analysis.cpp)
target_include_directories(ndi_recv_probe PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(ndi_recv_probe PRIVATE Qt6::Core olr_warnings olr_sanitize)
```

- [ ] **Step 3: Build the probe (build-only; running needs a runtime + source)**

Run: `cmake -S . -B build/claude-debug -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON` then `~/Qt/Tools/Ninja/ninja -C build/claude-debug ndi_recv_probe`
Expected: links `tests/e2e/ndi_recv_probe`. (If the NDI runtime is installed locally you may run `./build/claude-debug/tests/e2e/ndi_recv_probe nonexistent 1` and expect exit 77 with a "no source" / "not available" message; this is optional.)

- [ ] **Step 4: Format and commit**

```bash
/opt/homebrew/opt/llvm/bin/clang-format -i tests/e2e/ndi_recv_probe.cpp
git add tests/e2e/ndi_recv_probe.cpp tests/e2e/CMakeLists.txt
git commit -m "test: add runtime-loaded NDI receiver probe harness"
```

---

### Task 4: Tier (a) sender harness (marker → real NdiOutputSink)

**Files:**
- Create: `tests/e2e/ndi_output_sender.cpp`
- Modify: `tests/e2e/CMakeLists.txt` (target only)

**Interfaces:**
- Consumes: `NdiOutputSink` + `OutputBusFrame` + `OutputTargetAssignment` + `FrameRate` + `MediaVideoFrame` + `MediaAudioFrame` (all via `playback/output/...`, linked through `olr_test_playback`); the Task 1 marker (`ndiMarkerLumaPlane`, `ndiMarkerAudioS16`, `NdiOutputMarkerConfig`).
- Produces: the `ndi_output_sender` executable (sends the marker over NDI for the duration).

- [ ] **Step 1: Write the sender**

Create `tests/e2e/ndi_output_sender.cpp`:

```cpp
// Tier-(a) sender: builds marker OutputBusFrames and submits them to a REAL NdiOutputSink at
// the nominal cadence for the run. The minimal output path (sink + transport only). Exits 77
// (SKIP) if the sink cannot start (no NDI runtime). usage: ndi_output_sender <source-name>
// <seconds>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <cstdio>

#include "playback/output/mediaframe.h"
#include "playback/output/ndisink.h"
#include "playback/output/outputbusengine.h"
#include "playback/output/outputtargetassignment.h"
#include "tests/e2e/ndi_output_marker.h"

namespace {
constexpr int kSkip = 77;

MediaVideoFrame markerVideo(const NdiOutputMarkerConfig& mk, qint64 frameIndex) {
    MediaVideoFrame v = MediaVideoFrame::solidYuv420p(mk.width, mk.height, 128, 128, 128);
    v.feedIndex = 0;
    v.ptsMs = frameIndex * 1000 * mk.fpsDen / mk.fpsNum;
    v.planeY = ndiMarkerLumaPlane(mk, frameIndex); // overwrite luma with the marker
    return v;
}

MediaAudioFrame markerAudio(const NdiOutputMarkerConfig& mk, qint64 frameIndex) {
    MediaAudioFrame a;
    a.feedIndex = 0;
    a.startSample = frameIndex * ndiMarkerSamplesPerFrame(mk);
    a.sampleRate = mk.sampleRate;
    a.channels = mk.channels;
    a.format = MediaSampleFormat::S16Interleaved;
    a.pcm = ndiMarkerAudioS16(mk, frameIndex);
    return a;
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 3) {
        fprintf(stderr, "usage: ndi_output_sender <source-name> <seconds>\n");
        return 2;
    }
    const QString senderName = QString::fromUtf8(argv[1]);
    const double seconds = QString::fromUtf8(argv[2]).toDouble();

    NdiOutputMarkerConfig mk;
    const FrameRate rate = FrameRate::fromFraction(mk.fpsNum, mk.fpsDen);

    OutputTargetAssignment assignment;
    assignment.id = QStringLiteral("ndi-output-sender");
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.kind = OutputTargetKind::Ndi;
    assignment.enabled = true;
    assignment.settings.insert(QStringLiteral("senderName"), senderName);

    NdiOutputSink sink;
    if (!sink.start(assignment, rate)) {
        fprintf(stderr, "[ndi_output_sender] NdiOutputSink start failed (no runtime?) - SKIP\n");
        return kSkip;
    }

    const double periodMs = 1000.0 * mk.fpsDen / mk.fpsNum;
    QElapsedTimer run;
    run.start();
    qint64 frameIndex = 0;
    while (run.elapsed() < qint64(seconds * 1000.0)) {
        OutputBusFrame frame;
        frame.bus = OutputBusId::feed(0);
        frame.outputFrameIndex = frameIndex;
        frame.sampledPlayheadMs = frame.video.ptsMs;
        frame.video = markerVideo(mk, frameIndex);
        frame.audio = markerAudio(mk, frameIndex);
        sink.submit(frame);
        ++frameIndex;
        const qint64 targetMs = qint64(frameIndex * periodMs);
        const qint64 sleepMs = targetMs - run.elapsed();
        if (sleepMs > 0) QThread::msleep(static_cast<unsigned long>(sleepMs));
    }
    sink.stop();
    fprintf(stderr, "[ndi_output_sender] sent %lld frames as '%s'\n", (long long) frameIndex,
            senderName.toUtf8().constData());
    return 0;
}
```

- [ ] **Step 2: Add the target to `tests/e2e/CMakeLists.txt`**

After the `ndi_recv_probe` target block, add:

```cmake
qt_add_executable(ndi_output_sender
    ndi_output_sender.cpp
    ndi_output_marker.cpp)
target_include_directories(ndi_output_sender PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(ndi_output_sender PRIVATE
    Qt6::Core Qt6::Multimedia Qt6::Gui olr_test_playback olr_warnings olr_sanitize)
```

- [ ] **Step 3: Build the sender**

Run: `~/Qt/Tools/Ninja/ninja -C build/claude-debug ndi_output_sender`
Expected: links `tests/e2e/ndi_output_sender`. (If a local NDI runtime is present, `./build/claude-debug/tests/e2e/ndi_output_sender "OLR Probe Test" 1` should send and exit 0; without it, exit 77. Optional.)

- [ ] **Step 4: Format and commit**

```bash
/opt/homebrew/opt/llvm/bin/clang-format -i tests/e2e/ndi_output_sender.cpp
git add tests/e2e/ndi_output_sender.cpp tests/e2e/CMakeLists.txt
git commit -m "test: add tier-(a) NDI output sender (marker -> real NdiOutputSink)"
```

---

### Task 5: Driver script + opt-in CTest gate

**Files:**
- Create: `tests/e2e/run_ndi_output_e2e.sh`
- Modify: `tests/e2e/CMakeLists.txt` (add_test + properties)
- Modify: `.githooks/pre-push` (exclude `ndi-output`)

**Interfaces:**
- Consumes: `ndi_output_sender` + `ndi_recv_probe` (their CLIs + the `NDIRECV` report line).
- Produces: CTest `e2e_ndi_output` under label `ndi-output` with `SKIP_RETURN_CODE 77`.

- [ ] **Step 1: Write the driver**

Create `tests/e2e/run_ndi_output_e2e.sh`:

```bash
#!/usr/bin/env bash
# Rung-5 tier (a): send a marker stream through a real NdiOutputSink and verify the captured
# NDI output is continuous, A-V synced, and steady. Opt-in (CTest label "ndi-output").
# SKIPs (exit 77) when no NDI runtime/source is available.
#
# Usage: run_ndi_output_e2e.sh <ndi_output_sender_exe> <ndi_recv_probe_exe>
set -uo pipefail
SKIP=77

SENDER="${1:?ndi_output_sender executable required}"
PROBE="${2:?ndi_recv_probe executable required}"
SECONDS_RUN="${OLR_NDI_OUTPUT_SECONDS:-6}"
SRC="OLR NDI Output Probe $$"

SENDER_PID=""
cleanup() { [ -n "$SENDER_PID" ] && kill "$SENDER_PID" 2>/dev/null; wait "$SENDER_PID" 2>/dev/null; }
trap cleanup EXIT

# Start the sender in the background; give it a moment to register the source.
"$SENDER" "$SRC" "$((SECONDS_RUN + 3))" &
SENDER_PID=$!
sleep 1
# If the sender already exited 77 (no runtime), skip.
if ! kill -0 "$SENDER_PID" 2>/dev/null; then
    wait "$SENDER_PID"; rc=$?
    if [ "$rc" = "$SKIP" ]; then echo "SKIP: NDI runtime not available (sender)"; exit "$SKIP"; fi
    echo "FAIL: sender exited early ($rc)"; exit 1
fi

OUT="$("$PROBE" "$SRC" "$SECONDS_RUN")"
rc=$?
echo "$OUT"
if [ "$rc" = "$SKIP" ]; then echo "SKIP: NDI runtime/source not available (probe)"; exit "$SKIP"; fi
if [ "$rc" != "0" ]; then echo "FAIL: probe error ($rc)"; exit 1; fi

line="$(grep '^NDIRECV ' <<<"$OUT" || true)"
[ -n "$line" ] || { echo "FAIL: no NDIRECV report"; exit 1; }
field() { sed -n "s/.*$1=\\([0-9.-]*\\).*/\\1/p" <<<"$line"; }

frames=$(field framesReceived); drops=$(field drops); dupes=$(field dupes)
reorders=$(field reorders); avsync=$(field avSyncMaxFrames); maxgap=$(field maxGapFrames)

fail=0
[ "${frames:-0}" -ge "$((SECONDS_RUN * 10))" ] || { echo "FAIL: too few frames ($frames)"; fail=1; }
[ "${drops:-1}" = "0" ]    || { echo "FAIL: drops=$drops"; fail=1; }
[ "${dupes:-1}" = "0" ]    || { echo "FAIL: dupes=$dupes"; fail=1; }
[ "${reorders:-1}" = "0" ] || { echo "FAIL: reorders=$reorders"; fail=1; }
[ "${avsync:-9}" -ge 0 ] && [ "${avsync:-9}" -le 1 ] || { echo "FAIL: avSyncMaxFrames=$avsync"; fail=1; }
[ "${maxgap:-9}" -le 2 ]   || { echo "FAIL: maxGapFrames=$maxgap"; fail=1; }

if [ "$fail" = "0" ]; then echo "PASS: NDI output continuity/sync/cadence OK"; exit 0; fi
echo "NDI OUTPUT VALIDATION FAILED"; exit 1
```

- [ ] **Step 2: Make executable and register the test**

`chmod +x tests/e2e/run_ndi_output_e2e.sh`

In `tests/e2e/CMakeLists.txt`, after the `ndi_output_sender` target, add:

```cmake
add_test(NAME e2e_ndi_output
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run_ndi_output_e2e.sh"
        "$<TARGET_FILE:ndi_output_sender>" "$<TARGET_FILE:ndi_recv_probe>")
set_tests_properties(e2e_ndi_output PROPERTIES
    LABELS "ndi-output"
    TIMEOUT 120
    RUN_SERIAL TRUE
    SKIP_RETURN_CODE 77)
```

- [ ] **Step 3: Exclude `ndi-output` from the pre-push gate**

In `.githooks/pre-push`, change the CTest exclusion (the `-LE '...'` line) to append `|ndi-output`, e.g. from `-LE 'sync-report|srt|native-apple-ingest'` to `-LE 'sync-report|srt|native-apple-ingest|ndi-output'`. (GitHub CI runs only `-L ci`, so the `ndi-output` label never runs there.)

- [ ] **Step 4: Reconfigure and verify selection + skip behavior**

```bash
cmake -S . -B build/claude-debug -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/Qt/Tools/Ninja/ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON
~/Qt/Tools/Ninja/ninja -C build/claude-debug ndi_output_sender ndi_recv_probe
ctest --test-dir build/claude-debug -N -L ndi-output
ctest --test-dir build/claude-debug -L ndi-output --output-on-failure
```
Expected: `-N -L ndi-output` lists exactly `e2e_ndi_output`. The run either PASSES (if a local NDI runtime is installed and the loopback works) or is reported **Skipped** (exit 77) on a machine without the NDI runtime — both are acceptable; a hard FAIL is not.

Confirm it is excluded from the default selection:
```bash
ctest --test-dir build/claude-debug -N -LE 'sync-report|srt|native-apple-ingest|ndi-output' | grep -c e2e_ndi_output
```
Expected: `0`.

- [ ] **Step 5: Commit**

```bash
git add tests/e2e/run_ndi_output_e2e.sh tests/e2e/CMakeLists.txt .githooks/pre-push
git commit -m "test: register opt-in NDI output gate (label: ndi-output, skip-77)"
```

---

### Task 6: Full verification

**Files:** none (verification only).

- [ ] **Step 1: Build all new targets + unit tests**

```bash
~/Qt/Tools/Ninja/ninja -C build/claude-debug \
  tst_ndioutputmarker tst_ndirecvanalysis ndi_recv_probe ndi_output_sender
```
Expected: all build.

- [ ] **Step 2: Run the new unit tests**

```bash
ctest --test-dir build/claude-debug -R 'tst_ndioutputmarker|tst_ndirecvanalysis' --output-on-failure
```
Expected: `100% tests passed`.

- [ ] **Step 3: Run the opt-in gate (pass or skip, not fail)**

```bash
ctest --test-dir build/claude-debug -L ndi-output --output-on-failure
```
Expected: `e2e_ndi_output` Passed (with a local NDI runtime) or Skipped (without). Not Failed.

- [ ] **Step 4: Confirm formatting and clean tree**

```bash
for f in tests/e2e/ndi_output_marker.cpp tests/e2e/ndi_output_marker.h \
  tests/e2e/ndi_recv_analysis.cpp tests/e2e/ndi_recv_analysis.h \
  tests/e2e/ndi_recv_probe.cpp tests/e2e/ndi_output_sender.cpp \
  tests/unit/tst_ndioutputmarker.cpp tests/unit/tst_ndirecvanalysis.cpp; do
    /opt/homebrew/opt/llvm/bin/clang-format --output-replacements-xml "$f" | grep -q "<replacement " && echo "NEEDS-FORMAT: $f" || true
done
git diff --check origin/main
git status --short
```
Expected: no `NEEDS-FORMAT` lines; `git diff --check` prints nothing; only intended files changed.

---

## Self-Review Checklist

- **Spec coverage:** receiver harness (Task 3), pixel-encoded counter + flash/beep marker (Task 1), tier-(a) sender → real `NdiOutputSink` (Task 4), continuity/A-V/cadence metrics (Tasks 2+5), opt-in `ndi-output` label + skip-77 gating (Task 5), runtime-loaded/no-SDK-build (Tasks 3-4 link only Qt/olr libs), pure analysis unit-tested (Tasks 1-2). Covered.
- **Placeholder scan:** no TBD/TODO; every code step shows complete code.
- **Type consistency:** `NdiOutputMarkerConfig` fields and the marker/analysis function names/signatures used by the probe (Task 3) and sender (Task 4) match their definitions in Tasks 1-2 (`ndiMarkerLumaPlane`, `ndiMarkerDecodeIndex`, `ndiMarkerDecodeFlash`, `ndiMarkerAudioS16`, `ndiMarkerAudioRmsFltp`, `ndiMarkerSamplesPerFrame`, `ndiAnalyzeContinuity`, `ndiAvSyncMaxFrames`, `ndiAnalyzeCadence`). The probe's `NdiOutputMarkerConfig mk` defaults must equal the sender's, since both rely on the shared defaults — both construct `NdiOutputMarkerConfig` with no overrides.
- **Honest scope:** no production changes; the worker NDI-output API + tiers (b)/(c) are explicitly deferred to later sub-projects.
```
