# Format Canon Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (- [ ]) syntax.

**Goal:** Own the canonical pixel-format model for the GPU-resident pipeline — the `FramePixelFormat` plane-count contract, the NV12 shader-deinterleave model, the compositor `Rgba8` working/output format, and the per-sink export conversion — and deliver the NV12-deinterleave + nearest-neighbor **integer reference reconstructor** (the golden oracle) that `gpu-compositor` will be validated against, landing *before* any compositor work.

**Architecture:** A new `format-canon` module (`playback/output/formatcanon.{h,cpp}`) provides three pure, host-side, integer-deterministic pieces: (1) plane-count + format-shape predicates over the keystone's `FramePixelFormat`; (2) `CpuPlanes`↔`CpuPlanes` conversions between `Nv12`, `Yuv420p` and `Rgba8` (BT.601/709 + video/full range from `ColorMetadata`); (3) the `Rgba8`-out reference reconstructor `referenceComposeGridRgba8()` that integer-reconstructs today's nearest-neighbor grid composite *through* an NV12 chroma round-trip, modelling the **two** rounding sources (NV12 chroma decimation and nearest-neighbor scaling) that D2 mandates. The CPU `Yuv420pCompositor` stays the permanent oracle; this module is the bridge that makes its goldens reusable to validate the future GPU shader exactly on WARP and ±1 LSB on a local-GPU lane.

**Tech Stack:** C++17, Qt 6 (Core/Test), the frame-handle keystone types (`FramePixelFormat`, `ColorMetadata`, `CpuPlanes`, `FrameHandle`), CMake + Ninja. No FFmpeg, no GPU/RHI dependency in this subproject — everything here is pure host integer code testable today under `ctest -L unit`.

## Global Constraints

Copied verbatim from the program spec (§1, §7) — these are project-wide rules every subproject obeys:

- **Keystone-first, strict zero-regression gates.** This subproject consumes the `frame-handle` keystone (Phase 1) and must not change any CPU-path golden value. The CPU compositor (`Yuv420pCompositor`) stays the permanent correctness reference; nothing here alters its output.
- **The CPU path stays default and is the permanent correctness reference + fallback.** No code here runs on the GPU; it is the *oracle* the GPU path is later measured against.
- **Everything ships behind capability flags; nothing changes default behavior.** No call site is rewired to use `format-canon` conversions in Phase 2 — the module is additive. The first consumer is the (later) `gpu-compositor` validation harness.
- **No throwaway prototypes — every artifact is production and stays in the tree.** The reference reconstructor is the permanent oracle-test path, retained forever (§9). It is not scaffolding.
- **Public-repo professionalism.** Code, comments, commit messages and PR text are published. Keep them self-contained: no secrets, no internal notes, no references to private history; document the present design, not past incidents.
- **Format changed lines only.** CI's Lint job checks clang-format on changed lines only; several engine files use hand-written Allman style. Do not reformat whole files. Use Homebrew LLVM `git-clang-format` against `origin/main` (see CLAUDE.md).
- **8-bit only.** Per §1 non-goals the pipeline is structurally 8-bit; `ColorMetadata.bitDepth` is carried fixed at 8. No 10-bit/P010/HDR path here.

## Build & test commands (from CLAUDE.md; run from the worktree root)

Worktree root for this work: `/Users/timo.korkalainen/Development/timo/OpenLiveReplay/.claude/worktrees/gpu-resident-pipeline-design`.

```sh
# configure once (fresh build dir)
cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON
# build one target
cmake --build build/c --target <target>
# run one unit test verbosely
ctest --test-dir build/c -R <name> --output-on-failure
# run the whole unit label (a shared-lib change can affect siblings)
ctest --test-dir build/c -L unit --output-on-failure
```

Qt Test runs headless with `QT_QPA_PLATFORM=offscreen` (set by the test harness; the unit tests here are `QTEST_GUILESS_MAIN` so they do not need a display).

## Dependency note — the consumed keystone contract

This subproject **consumes** the `frame-handle` keystone (Phase 1) and adds to it. The keystone already provides, in `playback/output/`:

- `playback/output/framepixelformat.h` — `enum class FramePixelFormat { Nv12 = 0, Yuv420p = 1, Rgba8 = 2 };` and `constexpr int planeCount(FramePixelFormat)` returning `Nv12 → 2`, `Yuv420p → 3`, `Rgba8 → 1`.
- `playback/output/colormetadata.h` — `enum class ColorMatrix { Bt601, Bt709, Bt2020 };`, `enum class ColorRange { Video, Full };`, and `struct ColorMetadata { ColorMatrix matrix = Bt709; ColorRange range = Video; ColorPrimaries primaries = Bt709; ColorTransfer transfer = Bt709; ChromaFormat chromaFormat = Yuv420; int bitDepth = 8; bool operator==/!=; };`.
- `playback/output/framehandle.h` — `struct CpuPlanes { FramePixelFormat format = Yuv420p; int width = 0; int height = 0; QByteArray plane[3]; int stride[3] = {0,0,0}; bool isValid() const; };`, `class FrameHandle`, `FrameHandle solidYuv420pHandle(int,int,uchar,uchar,uchar);`, and `static FrameHandle Yuv420pCompositor::composeGrid(const QList<FrameHandle>&, int, int);`.

If, when implementation begins, the keystone has *not* yet landed those exact headers, **stop and resolve the dependency** — do not redefine `FramePixelFormat`/`CpuPlanes`/`ColorMetadata` here (that would fork the contract). This plan assumes they exist as above and only *adds* the conversion + reconstruction code in new `formatcanon.{h,cpp}` files.

## Phase ordering note (per spec §7)

- **Tasks 1–6 are pure CPU reference code, testable now** under `ctest -L unit`. They have no GPU dependency and constitute the deliverable that must land **before `gpu-compositor`** (spec §6: "delivers the NV12-deinterleave + nearest-neighbor integer reference reconstructor (the golden oracle) BEFORE `gpu-compositor` lands").
- **Task 7 (GPU-format enforcement)** documents the compositor working/output `Rgba8` contract and the per-sink export-format mapping as a *spec'd, asserted contract* that the Phase-2 `gpu-abstraction`/`gpu-compositor` work consumes. It introduces no GPU code; it pins the format expectations with a contract test so the later GPU tasks cannot silently diverge. Its enforcement at real sink call sites is Phase-2/Phase-4 work owned by `gpu-compositor`/`async-readback`, noted but not done here.

---

## Task 1: Format-shape predicates over the keystone `FramePixelFormat`

Plane-count itself is owned by the keystone (`planeCount()` in `framepixelformat.h`). This task adds the *derived* geometry helpers `format-canon` owns — the per-plane width/height/stride shape every conversion and the reconstructor need — as pure functions, with a test that pins them against the keystone's `planeCount` so the two never drift.

**Files:**
- Create: `playback/output/formatcanon.h`, `playback/output/formatcanon.cpp`
- Test: `tests/unit/tst_formatcanon.cpp`
- Modify: `CMakeLists.txt` (add to the app `OpenLiveReplay` source list), `tests/CMakeLists.txt` (add to `olr_test_playback`), `tests/unit/CMakeLists.txt` (register the test)

**Interfaces:**
- Consumes: `FramePixelFormat`, `planeCount(FramePixelFormat)` from `playback/output/framepixelformat.h`.
- Produces:
  ```cpp
  namespace formatcanon {
  // Per-plane sample geometry for an 8-bit packed (no padding) CpuPlanes of this
  // format at width x height. For an absent plane (index >= planeCount) returns {0,0}.
  struct PlaneShape { int width = 0; int height = 0; };
  PlaneShape planeShape(FramePixelFormat format, int frameWidth, int frameHeight, int planeIndex);
  // Tightly-packed stride (bytes per row) for that plane: planeShape().width * bytesPerSample.
  int packedStride(FramePixelFormat format, int frameWidth, int frameHeight, int planeIndex);
  // Bytes per sample in a plane of this format (Rgba8 plane 0 = 4; YUV planes = 1).
  int bytesPerSample(FramePixelFormat format, int planeIndex);
  } // namespace formatcanon
  ```
  Geometry rules (8-bit, 4:2:0 chroma decimation matching today's `MediaVideoFrame::solidYuv420p` and `Yuv420pCompositor`): for `Yuv420p` plane 0 is `w x h`, planes 1/2 are `((w+1)/2) x ((h+1)/2)`. For `Nv12` plane 0 (Y) is `w x h`, plane 1 (interleaved UV) is `((w+1)/2)` chroma-pairs wide → `2*((w+1)/2)` bytes wide x `((h+1)/2)` high; `bytesPerSample(Nv12, 1)` returns 1 (it is a byte plane; the `2*` is folded into `planeShape(...,1).width`). For `Rgba8` plane 0 is `w x h` at 4 bytes/sample.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_formatcanon.cpp`:

```cpp
// Unit tests for the format-canon geometry predicates: per-plane shape and
// packed stride for Nv12 / Yuv420p / Rgba8, pinned against the keystone's
// planeCount() so the two models cannot drift.
#include <QtTest>

#include "playback/output/formatcanon.h"
#include "playback/output/framepixelformat.h"

class TestFormatCanon : public QObject {
    Q_OBJECT
private slots:
    void planeShapeMatchesYuv420pLayout();
    void planeShapeMatchesNv12Layout();
    void planeShapeMatchesRgba8Layout();
    void absentPlanesAreZero();
    void packedStrideIsWidthTimesBytesPerSample();
};

void TestFormatCanon::planeShapeMatchesYuv420pLayout() {
    using namespace formatcanon;
    QCOMPARE(planeCount(FramePixelFormat::Yuv420p), 3);
    const PlaneShape y = planeShape(FramePixelFormat::Yuv420p, 8, 6, 0);
    QCOMPARE(y.width, 8);
    QCOMPARE(y.height, 6);
    const PlaneShape u = planeShape(FramePixelFormat::Yuv420p, 8, 6, 1);
    QCOMPARE(u.width, 4);  // (8+1)/2
    QCOMPARE(u.height, 3); // (6+1)/2
    const PlaneShape v = planeShape(FramePixelFormat::Yuv420p, 8, 6, 2);
    QCOMPARE(v.width, 4);
    QCOMPARE(v.height, 3);
    // Odd geometry rounds up (matches MediaVideoFrame::solidYuv420p).
    QCOMPARE(planeShape(FramePixelFormat::Yuv420p, 5, 5, 1).width, 3);  // (5+1)/2
    QCOMPARE(planeShape(FramePixelFormat::Yuv420p, 5, 5, 1).height, 3);
}

void TestFormatCanon::planeShapeMatchesNv12Layout() {
    using namespace formatcanon;
    QCOMPARE(planeCount(FramePixelFormat::Nv12), 2);
    const PlaneShape y = planeShape(FramePixelFormat::Nv12, 8, 6, 0);
    QCOMPARE(y.width, 8);
    QCOMPARE(y.height, 6);
    // Interleaved UV plane: (8+1)/2 = 4 chroma pairs -> 8 bytes wide, 3 rows.
    const PlaneShape uv = planeShape(FramePixelFormat::Nv12, 8, 6, 1);
    QCOMPARE(uv.width, 8);
    QCOMPARE(uv.height, 3);
    QCOMPARE(bytesPerSample(FramePixelFormat::Nv12, 1), 1);
}

void TestFormatCanon::planeShapeMatchesRgba8Layout() {
    using namespace formatcanon;
    QCOMPARE(planeCount(FramePixelFormat::Rgba8), 1);
    const PlaneShape p = planeShape(FramePixelFormat::Rgba8, 8, 6, 0);
    QCOMPARE(p.width, 8);
    QCOMPARE(p.height, 6);
    QCOMPARE(bytesPerSample(FramePixelFormat::Rgba8, 0), 4);
    QCOMPARE(packedStride(FramePixelFormat::Rgba8, 8, 6, 0), 32); // 8 * 4
}

void TestFormatCanon::absentPlanesAreZero() {
    using namespace formatcanon;
    const PlaneShape p = planeShape(FramePixelFormat::Rgba8, 8, 6, 1); // Rgba8 has 1 plane
    QCOMPARE(p.width, 0);
    QCOMPARE(p.height, 0);
    QCOMPARE(planeShape(FramePixelFormat::Nv12, 8, 6, 2).width, 0); // Nv12 has 2 planes
}

void TestFormatCanon::packedStrideIsWidthTimesBytesPerSample() {
    using namespace formatcanon;
    QCOMPARE(packedStride(FramePixelFormat::Yuv420p, 8, 6, 0), 8);   // 8 * 1
    QCOMPARE(packedStride(FramePixelFormat::Yuv420p, 8, 6, 1), 4);   // 4 * 1
    QCOMPARE(packedStride(FramePixelFormat::Nv12, 8, 6, 1), 8);      // 8 bytes (interleaved)
}

QTEST_GUILESS_MAIN(TestFormatCanon)
#include "tst_formatcanon.moc"
```

Register it in `tests/unit/CMakeLists.txt` immediately after the `tst_yuv420pcompositor` line (line 93):

```cmake
olr_add_unit_test(tst_formatcanon olr_test_playback)
```

- [ ] **Step 2: Run the test to verify it fails**

```sh
cmake --build build/c --target tst_formatcanon
```
Expected: FAIL to compile — `playback/output/formatcanon.h` not found (`fatal error: 'playback/output/formatcanon.h' file not found`).

- [ ] **Step 3: Write the minimal implementation**

Create `playback/output/formatcanon.h`:

```cpp
#ifndef FORMATCANON_H
#define FORMATCANON_H

#include "playback/output/framepixelformat.h"

// format-canon: the canonical pixel-format geometry + conversion + reference
// reconstruction model for the GPU-resident pipeline. Pure host integer code;
// no GPU/RHI dependency. The keystone owns planeCount(); this header owns the
// derived per-plane shape, the cross-format CpuPlanes conversions, and the
// nearest-neighbor + NV12 integer reference reconstructor (the golden oracle).
namespace formatcanon {

struct PlaneShape {
    int width = 0;
    int height = 0;
};

// Sample geometry of one plane of an 8-bit packed CpuPlanes at frameWidth x
// frameHeight. Chroma is 4:2:0 (round-up half resolution), matching today's
// MediaVideoFrame::solidYuv420p and Yuv420pCompositor. For Nv12 the UV plane is
// reported as a byte plane: width is the byte width (2 bytes per chroma pair).
// planeIndex >= planeCount(format) returns {0, 0}.
PlaneShape planeShape(FramePixelFormat format, int frameWidth, int frameHeight, int planeIndex);

// Bytes per sample in a plane (Rgba8 plane 0 = 4; all YUV byte planes = 1).
int bytesPerSample(FramePixelFormat format, int planeIndex);

// Tightly-packed bytes-per-row: planeShape(...).width is already in samples for
// YUV (1 byte each) or bytes for Nv12 UV / Rgba8 packing handled by
// bytesPerSample. packedStride = planeShape.width * bytesPerSample, except for
// Nv12 UV whose width is already a byte width (bytesPerSample == 1).
int packedStride(FramePixelFormat format, int frameWidth, int frameHeight, int planeIndex);

} // namespace formatcanon

#endif // FORMATCANON_H
```

Create `playback/output/formatcanon.cpp`:

```cpp
#include "playback/output/formatcanon.h"

namespace formatcanon {

namespace {
int chromaWidth(int frameWidth) { return (frameWidth + 1) / 2; }
int chromaHeight(int frameHeight) { return (frameHeight + 1) / 2; }
} // namespace

PlaneShape planeShape(FramePixelFormat format, int frameWidth, int frameHeight, int planeIndex) {
    if (planeIndex < 0 || planeIndex >= planeCount(format)) return {0, 0};
    switch (format) {
    case FramePixelFormat::Yuv420p:
        if (planeIndex == 0) return {frameWidth, frameHeight};
        return {chromaWidth(frameWidth), chromaHeight(frameHeight)};
    case FramePixelFormat::Nv12:
        if (planeIndex == 0) return {frameWidth, frameHeight};
        // Interleaved UV: chromaWidth pairs * 2 bytes wide, chromaHeight rows.
        return {2 * chromaWidth(frameWidth), chromaHeight(frameHeight)};
    case FramePixelFormat::Rgba8:
        return {frameWidth, frameHeight};
    }
    return {0, 0};
}

int bytesPerSample(FramePixelFormat format, int planeIndex) {
    if (format == FramePixelFormat::Rgba8 && planeIndex == 0) return 4;
    return 1;
}

int packedStride(FramePixelFormat format, int frameWidth, int frameHeight, int planeIndex) {
    const PlaneShape s = planeShape(format, frameWidth, frameHeight, planeIndex);
    return s.width * bytesPerSample(format, planeIndex);
}

} // namespace formatcanon
```

Add the files to the app source list in `CMakeLists.txt`, right after the `mediaframe.h` line (line 192):

```cmake
        playback/output/framepixelformat.h
        playback/output/formatcanon.h playback/output/formatcanon.cpp
```

(`framepixelformat.h` is listed by the keystone already — if it is present, add only the `formatcanon.h/.cpp` line.)

Add `formatcanon.cpp` to the `olr_test_playback` library in `tests/CMakeLists.txt`, right after the `yuv420pcompositor.cpp` line (line 227):

```cmake
    "${CMAKE_SOURCE_DIR}/playback/output/formatcanon.cpp"
```

- [ ] **Step 4: Run the test to verify it passes**

```sh
cmake --build build/c --target tst_formatcanon && ctest --test-dir build/c -R tst_formatcanon --output-on-failure
```
Expected: PASS (5 test functions).

- [ ] **Step 5: Commit**

```sh
git add playback/output/formatcanon.h playback/output/formatcanon.cpp \
        tests/unit/tst_formatcanon.cpp tests/unit/CMakeLists.txt \
        tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat(format-canon): plane-shape geometry predicates over FramePixelFormat"
```

---

## Task 2: NV12 ↔ Yuv420p CpuPlanes deinterleave/interleave (lossless byte-exact)

The on-GPU canonical layout is NV12 (D2). The reference reconstructor and every sink-export step needs the integer-exact NV12↔I420 chroma (de)interleave. The keystone's `CpuFrameData::readToCpu` does this conversion internally; `format-canon` owns the **standalone, testable** integer kernel that the reconstructor reuses and that the keystone can be checked against. This step is *lossless* (NV12↔I420 only reorders chroma bytes — no decimation), so it round-trips byte-for-byte.

**Files:**
- Modify: `playback/output/formatcanon.h`, `playback/output/formatcanon.cpp`
- Test: `tests/unit/tst_formatcanon.cpp`

**Interfaces:**
- Consumes: `CpuPlanes` from `playback/output/framehandle.h`; `formatcanon::planeShape`, `formatcanon::packedStride` (Task 1).
- Produces:
  ```cpp
  namespace formatcanon {
  // Deinterleave an Nv12 CpuPlanes (Y + interleaved UV) into a tightly-packed
  // Yuv420p CpuPlanes (Y + U + V). Returns an invalid CpuPlanes (isValid()==false)
  // if src.format != Nv12 or src is invalid. Lossless: chroma bytes are reordered,
  // not resampled.
  CpuPlanes nv12ToYuv420p(const CpuPlanes& src);
  // Interleave a Yuv420p CpuPlanes into a tightly-packed Nv12 CpuPlanes. Returns
  // invalid if src.format != Yuv420p or src is invalid. Lossless inverse of
  // nv12ToYuv420p for tightly-packed input.
  CpuPlanes yuv420pToNv12(const CpuPlanes& src);
  } // namespace formatcanon
  ```

- [ ] **Step 1: Write the failing test**

Add to `tests/unit/tst_formatcanon.cpp` — declare two slots and include the keystone header at the top:

```cpp
#include "playback/output/framehandle.h"  // CpuPlanes
```

```cpp
    void nv12DeinterleaveProducesPlanarUV();
    void nv12RoundTripsByteExact();
```

```cpp
// Build a tightly-packed Nv12 CpuPlanes with a known UV interleave pattern so a
// swapped U/V or an off-by-one chroma stride is detectable.
static CpuPlanes makeNv12_4x4() {
    CpuPlanes p;
    p.format = FramePixelFormat::Nv12;
    p.width = 4;
    p.height = 4;
    p.stride[0] = 4;          // Y: 4x4
    p.stride[1] = 4;          // UV: (4+1)/2=2 pairs -> 4 bytes wide, 2 rows
    p.plane[0] = QByteArray(16, char(100));
    // UV plane is U0 V0 U1 V1 / U2 V2 U3 V3 (2 rows of 2 pairs). Distinct values.
    QByteArray uv(8, 0);
    const uchar us[4] = {10, 11, 12, 13};
    const uchar vs[4] = {200, 201, 202, 203};
    for (int i = 0; i < 4; ++i) {
        uv[2 * i] = char(us[i]);
        uv[2 * i + 1] = char(vs[i]);
    }
    p.plane[1] = uv;
    return p;
}

void TestFormatCanon::nv12DeinterleaveProducesPlanarUV() {
    const CpuPlanes out = formatcanon::nv12ToYuv420p(makeNv12_4x4());
    QVERIFY(out.isValid());
    QCOMPARE(int(out.format), int(FramePixelFormat::Yuv420p));
    QCOMPARE(out.width, 4);
    QCOMPARE(out.height, 4);
    QCOMPARE(out.stride[1], 2); // (4+1)/2
    QCOMPARE(out.stride[2], 2);
    QCOMPARE(out.plane[0], QByteArray(16, char(100)));
    // U plane = de-interleaved evens; V plane = odds.
    const uchar usExp[4] = {10, 11, 12, 13};
    const uchar vsExp[4] = {200, 201, 202, 203};
    for (int i = 0; i < 4; ++i) {
        QCOMPARE(uchar(out.plane[1].at(i)), usExp[i]);
        QCOMPARE(uchar(out.plane[2].at(i)), vsExp[i]);
    }
}

void TestFormatCanon::nv12RoundTripsByteExact() {
    const CpuPlanes nv12 = makeNv12_4x4();
    const CpuPlanes planar = formatcanon::nv12ToYuv420p(nv12);
    const CpuPlanes back = formatcanon::yuv420pToNv12(planar);
    QVERIFY(back.isValid());
    QCOMPARE(int(back.format), int(FramePixelFormat::Nv12));
    QCOMPARE(back.plane[0], nv12.plane[0]);
    QCOMPARE(back.plane[1], nv12.plane[1]);
    // Wrong-format inputs are rejected.
    QVERIFY(!formatcanon::nv12ToYuv420p(planar).isValid()); // planar is Yuv420p
    QVERIFY(!formatcanon::yuv420pToNv12(nv12).isValid());    // nv12 is Nv12
}
```

- [ ] **Step 2: Run the test to verify it fails**

```sh
cmake --build build/c --target tst_formatcanon
```
Expected: FAIL to compile — `formatcanon::nv12ToYuv420p` / `yuv420pToNv12` undeclared (`no member named 'nv12ToYuv420p' in namespace 'formatcanon'`).

- [ ] **Step 3: Write the minimal implementation**

Add `#include "playback/output/framehandle.h"` to `formatcanon.h` (for `CpuPlanes`) and append the declarations:

```cpp
CpuPlanes nv12ToYuv420p(const CpuPlanes& src);
CpuPlanes yuv420pToNv12(const CpuPlanes& src);
```

In `formatcanon.cpp` add (the `<cstring>` include at top for `memcpy` is not needed — this is byte-indexed):

```cpp
CpuPlanes nv12ToYuv420p(const CpuPlanes& src) {
    if (src.format != FramePixelFormat::Nv12 || !src.isValid()) return CpuPlanes{};
    const int w = src.width;
    const int h = src.height;
    const int cw = (w + 1) / 2;
    const int ch = (h + 1) / 2;

    CpuPlanes out;
    out.format = FramePixelFormat::Yuv420p;
    out.width = w;
    out.height = h;
    out.stride[0] = w;
    out.stride[1] = cw;
    out.stride[2] = cw;
    out.plane[0] = QByteArray(w * h, 0);
    out.plane[1] = QByteArray(cw * ch, 0);
    out.plane[2] = QByteArray(cw * ch, 0);

    for (int y = 0; y < h; ++y) {
        const char* srcRow = src.plane[0].constData() + y * src.stride[0];
        char* dstRow = out.plane[0].data() + y * w;
        for (int x = 0; x < w; ++x) dstRow[x] = srcRow[x];
    }
    for (int y = 0; y < ch; ++y) {
        const char* srcRow = src.plane[1].constData() + y * src.stride[1];
        char* uRow = out.plane[1].data() + y * cw;
        char* vRow = out.plane[2].data() + y * cw;
        for (int x = 0; x < cw; ++x) {
            uRow[x] = srcRow[2 * x];
            vRow[x] = srcRow[2 * x + 1];
        }
    }
    return out;
}

CpuPlanes yuv420pToNv12(const CpuPlanes& src) {
    if (src.format != FramePixelFormat::Yuv420p || !src.isValid()) return CpuPlanes{};
    const int w = src.width;
    const int h = src.height;
    const int cw = (w + 1) / 2;
    const int ch = (h + 1) / 2;

    CpuPlanes out;
    out.format = FramePixelFormat::Nv12;
    out.width = w;
    out.height = h;
    out.stride[0] = w;
    out.stride[1] = 2 * cw;
    out.plane[0] = QByteArray(w * h, 0);
    out.plane[1] = QByteArray(2 * cw * ch, 0);

    for (int y = 0; y < h; ++y) {
        const char* srcRow = src.plane[0].constData() + y * src.stride[0];
        char* dstRow = out.plane[0].data() + y * w;
        for (int x = 0; x < w; ++x) dstRow[x] = srcRow[x];
    }
    for (int y = 0; y < ch; ++y) {
        const char* uRow = src.plane[1].constData() + y * src.stride[1];
        const char* vRow = src.plane[2].constData() + y * src.stride[2];
        char* dstRow = out.plane[1].data() + y * (2 * cw);
        for (int x = 0; x < cw; ++x) {
            dstRow[2 * x] = uRow[x];
            dstRow[2 * x + 1] = vRow[x];
        }
    }
    return out;
}
```

- [ ] **Step 4: Run the test to verify it passes**

```sh
cmake --build build/c --target tst_formatcanon && ctest --test-dir build/c -R tst_formatcanon --output-on-failure
```
Expected: PASS (7 test functions).

- [ ] **Step 5: Commit**

```sh
git add playback/output/formatcanon.h playback/output/formatcanon.cpp tests/unit/tst_formatcanon.cpp
git commit -m "feat(format-canon): lossless NV12<->Yuv420p chroma (de)interleave"
```

---

## Task 3: NV12 chroma round-trip (the lossy 4:2:0 decimation rounding source)

NV12 storage of an arbitrary `Yuv420p` source is byte-exact (Task 2), but the spec's "NV12 double-rounding" (D2, §9) refers to a *different* rounding source the reconstructor must model: when the GPU samples NV12 chroma and the reference must agree, chroma is at half resolution and **a luma-resolution lookup picks one chroma sample per 2×2 luma block** — i.e. the chroma plane is shared by a 2×2 luma quad. The reference reconstructor reads chroma at `(x>>1, y>>1)`. This task adds the integer chroma-upsample-by-replication kernel (`Yuv420p` planar → full-resolution per-luma-pixel U/V), the rounding source that stacks on nearest-neighbor scaling in Task 5.

**Files:**
- Modify: `playback/output/formatcanon.h`, `playback/output/formatcanon.cpp`
- Test: `tests/unit/tst_formatcanon.cpp`

**Interfaces:**
- Consumes: `CpuPlanes` (keystone); Task 1/2 helpers.
- Produces:
  ```cpp
  namespace formatcanon {
  // Sample the chroma of a Yuv420p (or Nv12-deinterleaved) CpuPlanes at full luma
  // resolution by nearest 2x2-block replication: U/V at luma pixel (x,y) come from
  // chroma sample (x>>1, y>>1) clamped to the chroma plane. Models the NV12 chroma
  // decimation rounding source (one chroma sample per 2x2 luma quad). Returns the
  // full-res U and V planes (w*h each) for src; src must be Yuv420p and valid.
  struct FullResChroma { int width = 0; int height = 0; QByteArray u; QByteArray v; };
  FullResChroma upsampleChromaNearest(const CpuPlanes& yuv420p);
  } // namespace formatcanon
  ```

- [ ] **Step 1: Write the failing test**

Add a slot to `tst_formatcanon.cpp`:

```cpp
    void chromaUpsampleReplicates2x2Blocks();
```

```cpp
void TestFormatCanon::chromaUpsampleReplicates2x2Blocks() {
    // 4x4 luma -> 2x2 chroma. Distinct chroma per 2x2 block so replication and
    // block boundaries are checkable.
    CpuPlanes p;
    p.format = FramePixelFormat::Yuv420p;
    p.width = 4;
    p.height = 4;
    p.stride[0] = 4;
    p.stride[1] = 2;
    p.stride[2] = 2;
    p.plane[0] = QByteArray(16, char(0));
    // chroma 2x2: U = [10 20 / 30 40], V = [110 120 / 130 140]
    p.plane[1] = QByteArrayLiteral("\x0a\x14\x1e\x28");          // 10 20 30 40
    p.plane[2] = QByteArray(4, 0);
    p.plane[2][0] = char(110); p.plane[2][1] = char(120);
    p.plane[2][2] = char(130); p.plane[2][3] = char(140);

    const formatcanon::FullResChroma c = formatcanon::upsampleChromaNearest(p);
    QCOMPARE(c.width, 4);
    QCOMPARE(c.height, 4);
    // Top-left 2x2 luma block all share chroma (10,110).
    auto uAt = [&](int x, int y) { return uchar(c.u.at(y * 4 + x)); };
    auto vAt = [&](int x, int y) { return uchar(c.v.at(y * 4 + x)); };
    QCOMPARE(uAt(0, 0), uchar(10)); QCOMPARE(uAt(1, 1), uchar(10));
    QCOMPARE(vAt(0, 0), uchar(110)); QCOMPARE(vAt(1, 1), uchar(110));
    // Top-right block (20,120).
    QCOMPARE(uAt(2, 0), uchar(20)); QCOMPARE(uAt(3, 1), uchar(20));
    QCOMPARE(vAt(3, 0), uchar(120));
    // Bottom-left (30,130), bottom-right (40,140).
    QCOMPARE(uAt(0, 2), uchar(30)); QCOMPARE(uAt(1, 3), uchar(30));
    QCOMPARE(uAt(2, 2), uchar(40)); QCOMPARE(uAt(3, 3), uchar(40));
    QCOMPARE(vAt(2, 3), uchar(140));
}
```

- [ ] **Step 2: Run the test to verify it fails**

```sh
cmake --build build/c --target tst_formatcanon
```
Expected: FAIL to compile — `formatcanon::upsampleChromaNearest` / `FullResChroma` undeclared.

- [ ] **Step 3: Write the minimal implementation**

Declarations in `formatcanon.h`:

```cpp
struct FullResChroma {
    int width = 0;
    int height = 0;
    QByteArray u;
    QByteArray v;
};
FullResChroma upsampleChromaNearest(const CpuPlanes& yuv420p);
```

In `formatcanon.cpp`:

```cpp
FullResChroma upsampleChromaNearest(const CpuPlanes& yuv420p) {
    if (yuv420p.format != FramePixelFormat::Yuv420p || !yuv420p.isValid()) return FullResChroma{};
    const int w = yuv420p.width;
    const int h = yuv420p.height;
    const int cw = (w + 1) / 2;
    const int ch = (h + 1) / 2;

    FullResChroma out;
    out.width = w;
    out.height = h;
    out.u = QByteArray(w * h, 0);
    out.v = QByteArray(w * h, 0);
    for (int y = 0; y < h; ++y) {
        const int cy = qMin(ch - 1, y >> 1);
        const char* uRow = yuv420p.plane[1].constData() + cy * yuv420p.stride[1];
        const char* vRow = yuv420p.plane[2].constData() + cy * yuv420p.stride[2];
        char* uDst = out.u.data() + y * w;
        char* vDst = out.v.data() + y * w;
        for (int x = 0; x < w; ++x) {
            const int cx = qMin(cw - 1, x >> 1);
            uDst[x] = uRow[cx];
            vDst[x] = vRow[cx];
        }
    }
    return out;
}
```

Add `#include <QtGlobal>` to `formatcanon.cpp` for `qMin` if not already pulled in transitively.

- [ ] **Step 4: Run the test to verify it passes**

```sh
cmake --build build/c --target tst_formatcanon && ctest --test-dir build/c -R tst_formatcanon --output-on-failure
```
Expected: PASS (8 test functions).

- [ ] **Step 5: Commit**

```sh
git add playback/output/formatcanon.h playback/output/formatcanon.cpp tests/unit/tst_formatcanon.cpp
git commit -m "feat(format-canon): nearest 2x2-block chroma upsample (NV12 decimation oracle)"
```

---

## Task 4: Integer YUV → Rgba8 color convert (BT.601/709, video/full range)

The compositor working/output format is `Rgba8` (D2). The reference reconstructor and the per-sink export both need the integer-exact YUV→RGB conversion the GPU shader will reproduce. This uses **fixed-point integer coefficients** (not float) so the WARP-exact requirement is meetable: the shader integer-reconstructs *this* math. `ColorMetadata.matrix` selects BT.601 vs BT.709; `ColorMetadata.range` selects video (16–235 / 16–240) vs full (0–255).

**Files:**
- Modify: `playback/output/formatcanon.h`, `playback/output/formatcanon.cpp`
- Test: `tests/unit/tst_formatcanon.cpp`

**Interfaces:**
- Consumes: `ColorMatrix`, `ColorRange` from `playback/output/colormetadata.h`.
- Produces:
  ```cpp
  namespace formatcanon {
  // Integer (Q8 fixed-point) YUV-sample -> 8-bit RGB. Deterministic, no float.
  // matrix selects BT.601/709 coefficients; range selects video(limited)/full
  // black/white levels. Output channels are clamped to [0,255].
  struct Rgb8 { uchar r = 0; uchar g = 0; uchar b = 0; };
  Rgb8 yuvToRgb8(uchar y, uchar u, uchar v, ColorMatrix matrix, ColorRange range);
  } // namespace formatcanon
  ```

Fixed-point reference (Q8, i.e. coefficients × 256, rounded; the GPU shader integer-reconstructs exactly these constants):
- Video range: `Yc = clamp(Y - 16, 0, 219)` scaled by `1192` (≈255/219×256÷... ) — concretely use `Y' = (Y - 16) * 1192`, `Cb = U - 128`, `Cr = V - 128`. Full range: `Y' = Y * 1024`, levels unshifted.
- BT.601: `R = Y' + 1634*Cr`, `G = Y' - 401*Cb - 832*Cr`, `B = Y' + 2066*Cb`.
- BT.709: `R = Y' + 1836*Cr`, `G = Y' - 218*Cb - 546*Cr`, `B = Y' + 2164*Cb`.
- Each result `>> 10`, then clamp to `[0,255]`. (Coefficients chosen so identical integer math is reproducible on WARP.)

> These exact constants are the contract: the GPU compat shader (in `gpu-compositor`) must use them verbatim. Document them in the header comment so the shader author copies them, not re-derives.

- [ ] **Step 1: Write the failing test**

Add to `tst_formatcanon.cpp`, including `#include "playback/output/colormetadata.h"`:

```cpp
    void yuvToRgbVideoRangeNeutralGrey();
    void yuvToRgbPrimariesDifferByMatrix();
    void yuvToRgbClampsAndFullRange();
```

```cpp
void TestFormatCanon::yuvToRgbVideoRangeNeutralGrey() {
    // Neutral grey: Y=126 (mid of 16..235 ~= 125.5), U=V=128 -> R==G==B, no chroma.
    const formatcanon::Rgb8 g =
        formatcanon::yuvToRgb8(126, 128, 128, ColorMatrix::Bt709, ColorRange::Video);
    QCOMPARE(g.r, g.g);
    QCOMPARE(g.g, g.b);
    // Y=16 video black -> ~0; Y=235 video white -> ~255.
    const formatcanon::Rgb8 black =
        formatcanon::yuvToRgb8(16, 128, 128, ColorMatrix::Bt709, ColorRange::Video);
    QCOMPARE(black.r, uchar(0));
    const formatcanon::Rgb8 white =
        formatcanon::yuvToRgb8(235, 128, 128, ColorMatrix::Bt709, ColorRange::Video);
    QCOMPARE(white.r, uchar(255));
}

void TestFormatCanon::yuvToRgbPrimariesDifferByMatrix() {
    // A coloured sample must convert differently under 601 vs 709 (proves the
    // matrix is actually consulted, not hard-coded).
    const formatcanon::Rgb8 c601 =
        formatcanon::yuvToRgb8(120, 90, 200, ColorMatrix::Bt601, ColorRange::Video);
    const formatcanon::Rgb8 c709 =
        formatcanon::yuvToRgb8(120, 90, 200, ColorMatrix::Bt709, ColorRange::Video);
    QVERIFY(c601.r != c709.r || c601.g != c709.g || c601.b != c709.b);
}

void TestFormatCanon::yuvToRgbClampsAndFullRange() {
    // Full-range black/white at the 0..255 endpoints.
    const formatcanon::Rgb8 fb =
        formatcanon::yuvToRgb8(0, 128, 128, ColorMatrix::Bt709, ColorRange::Full);
    QCOMPARE(fb.r, uchar(0));
    const formatcanon::Rgb8 fw =
        formatcanon::yuvToRgb8(255, 128, 128, ColorMatrix::Bt709, ColorRange::Full);
    QCOMPARE(fw.r, uchar(255));
    // Extreme chroma must clamp, never wrap past 255 or below 0.
    const formatcanon::Rgb8 hot =
        formatcanon::yuvToRgb8(235, 16, 240, ColorMatrix::Bt601, ColorRange::Video);
    QVERIFY(hot.r <= 255 && hot.g <= 255 && hot.b <= 255);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```sh
cmake --build build/c --target tst_formatcanon
```
Expected: FAIL to compile — `formatcanon::yuvToRgb8` / `Rgb8` undeclared.

- [ ] **Step 3: Write the minimal implementation**

Declarations in `formatcanon.h` (add `#include "playback/output/colormetadata.h"`):

```cpp
struct Rgb8 {
    uchar r = 0;
    uchar g = 0;
    uchar b = 0;
};
// Integer (Q10 fixed-point) YUV -> 8-bit RGB. Deterministic, no float; the GPU
// compat shader MUST integer-reconstruct exactly these constants for WARP-exact
// agreement. matrix = BT.601/709; range = video(limited)/full.
Rgb8 yuvToRgb8(uchar y, uchar u, uchar v, ColorMatrix matrix, ColorRange range);
```

In `formatcanon.cpp`:

```cpp
namespace {
uchar clampU8(int v) { return uchar(v < 0 ? 0 : (v > 255 ? 255 : v)); }
} // namespace

Rgb8 yuvToRgb8(uchar y, uchar u, uchar v, ColorMatrix matrix, ColorRange range) {
    // Q10 fixed point. Luma scaled so that the range endpoints map to 0..255.
    int yp;
    if (range == ColorRange::Video) {
        // (Y-16) * (255/219) in Q10 == (Y-16) * 1192.
        yp = (int(y) - 16) * 1192;
    } else {
        // Y * (255/255) in Q10 == Y * 1024.
        yp = int(y) * 1024;
    }
    const int cb = int(u) - 128;
    const int cr = int(v) - 128;

    int r, g, b;
    if (matrix == ColorMatrix::Bt601) {
        r = yp + 1634 * cr;
        g = yp - 401 * cb - 832 * cr;
        b = yp + 2066 * cb;
    } else { // Bt709 (and Bt2020 not used at 8-bit here -> treat as 709 spine)
        r = yp + 1836 * cr;
        g = yp - 218 * cb - 546 * cr;
        b = yp + 2164 * cb;
    }
    Rgb8 out;
    out.r = clampU8((r + 512) >> 10);
    out.g = clampU8((g + 512) >> 10);
    out.b = clampU8((b + 512) >> 10);
    return out;
}
```

> The `+512` is round-to-nearest for the Q10 shift; the shader author replicates it. If the neutral-grey test shows `black.r != 0` or `white.r != 255` off by one, adjust the video-range luma scale constant (1192) and the rounding bias together and re-run — the test pins the endpoints. Do not switch to float to "fix" rounding; WARP-exactness requires the integer path.

- [ ] **Step 4: Run the test to verify it passes**

```sh
cmake --build build/c --target tst_formatcanon && ctest --test-dir build/c -R tst_formatcanon --output-on-failure
```
Expected: PASS (11 test functions).

- [ ] **Step 5: Commit**

```sh
git add playback/output/formatcanon.h playback/output/formatcanon.cpp tests/unit/tst_formatcanon.cpp
git commit -m "feat(format-canon): integer BT.601/709 + range YUV->Rgba8 convert"
```

---

## Task 5: The reference reconstructor — Rgba8 grid composite with NV12 + nearest-neighbor double rounding

This is the deliverable that must land **before `gpu-compositor`**: the golden oracle. It composites a grid of `FrameHandle` sources into an `Rgba8` output, modelling **both** rounding sources D2/§9 demand: (1) the source goes through an NV12 chroma round-trip (chroma at 2×2-block resolution, Task 3 upsample), and (2) the grid scaling is nearest-neighbor (today's `scalePlaneNearest` math, `srcX = (x*srcW)/dstW`). The output is `Rgba8` via the Task-4 color convert. The GPU "compat shader" will be validated against this — exact on WARP, ±1 LSB on local-GPU.

The grid geometry (columns/rows, tile rects) is byte-identical to `Yuv420pCompositor::composeGrid` (`yuv420pcompositor.cpp:23-61`) so a side test can cross-check that the *luma placement* of this reconstructor agrees with the retained CPU oracle.

**Files:**
- Modify: `playback/output/formatcanon.h`, `playback/output/formatcanon.cpp`
- Test: `tests/unit/tst_formatcanon.cpp`

**Interfaces:**
- Consumes: `FrameHandle`, `FrameHandle::readToCpu(FramePixelFormat)` (keystone); `formatcanon::yuv420pToNv12`, `nv12ToYuv420p`, `upsampleChromaNearest`, `yuvToRgb8` (Tasks 2–4).
- Produces:
  ```cpp
  namespace formatcanon {
  // The golden oracle. Composites the grid exactly as Yuv420pCompositor::composeGrid
  // (same columns/rows/tile rects, nearest-neighbor scaling) BUT each source first
  // round-trips through NV12 (chroma decimated to 2x2-block resolution) and the
  // output is Rgba8 via integer BT.601/709 + range convert. Two rounding sources:
  // NV12 chroma decimation + nearest-neighbor scale. color selects the convert.
  // Returns an Rgba8 CpuPlanes (plane[0] packed RGBA, stride = width*4).
  CpuPlanes referenceComposeGridRgba8(const QList<FrameHandle>& frames, int width, int height,
                                      ColorMetadata color);
  } // namespace formatcanon
  ```

- [ ] **Step 1: Write the failing test**

Add to `tst_formatcanon.cpp`, including `#include "playback/output/framehandle.h"` (already added) and `#include "playback/output/yuv420pcompositor.h"`:

```cpp
    void referenceGridPlacesTilesLikeCpuOracleLuma();
    void referenceGridModelsNv12ChromaDecimation();
    void referenceGridOutputIsRgba8();
```

```cpp
// Reconstruct expected RGBA for a flat solid tile through the same two rounding
// sources, computed independently of referenceComposeGridRgba8's loop.
static formatcanon::Rgb8 expectedSolidRgb(uchar y, uchar u, uchar v, ColorMetadata c) {
    // A flat source survives NV12 chroma decimation unchanged (every chroma cell
    // equal), so the expected RGB is just the direct convert.
    return formatcanon::yuvToRgb8(y, u, v, c.matrix, c.range);
}

void TestFormatCanon::referenceGridPlacesTilesLikeCpuOracleLuma() {
    // Four flat sources, 2x2 grid into 8x8. The luma TILE BOUNDARIES of the
    // reconstructor must match the CPU oracle's (placement is shared geometry).
    QList<FrameHandle> frames{solidYuv420pHandle(4, 4, 40, 60, 200),
                              solidYuv420pHandle(4, 4, 80, 70, 190),
                              solidYuv420pHandle(4, 4, 120, 80, 180),
                              solidYuv420pHandle(4, 4, 160, 90, 170)};
    ColorMetadata c; // defaults: Bt709 / Video
    const CpuPlanes rgba = formatcanon::referenceComposeGridRgba8(frames, 8, 8, c);
    QVERIFY(rgba.isValid());
    QCOMPARE(int(rgba.format), int(FramePixelFormat::Rgba8));
    QCOMPARE(rgba.width, 8);
    QCOMPARE(rgba.height, 8);

    auto pixelRgb = [&](int x, int y) {
        const int o = y * rgba.stride[0] + x * 4;
        return formatcanon::Rgb8{uchar(rgba.plane[0].at(o)), uchar(rgba.plane[0].at(o + 1)),
                                 uchar(rgba.plane[0].at(o + 2))};
    };
    const formatcanon::Rgb8 tl = expectedSolidRgb(40, 60, 200, c);
    const formatcanon::Rgb8 tr = expectedSolidRgb(80, 70, 190, c);
    const formatcanon::Rgb8 bl = expectedSolidRgb(120, 80, 180, c);
    const formatcanon::Rgb8 br = expectedSolidRgb(160, 90, 170, c);
    auto eq = [](formatcanon::Rgb8 a, formatcanon::Rgb8 b) {
        return a.r == b.r && a.g == b.g && a.b == b.b;
    };
    QVERIFY(eq(pixelRgb(0, 0), tl));   // top-left tile
    QVERIFY(eq(pixelRgb(4, 0), tr));   // top-right tile starts at x=4
    QVERIFY(eq(pixelRgb(0, 4), bl));   // bottom-left at y=4
    QVERIFY(eq(pixelRgb(4, 4), br));
    // Alpha is opaque.
    QCOMPARE(uchar(rgba.plane[0].at(3)), uchar(255));
}

void TestFormatCanon::referenceGridModelsNv12ChromaDecimation() {
    // A source whose chroma VARIES at luma resolution must show 2x2-block chroma
    // (decimation) in the reconstructed output, NOT per-pixel chroma. Build a 2x2
    // single-tile grid (count=1 -> 1x1 grid filling the frame) with a chroma edge
    // that decimation collapses.
    CpuPlanes p;
    p.format = FramePixelFormat::Yuv420p;
    p.width = 2;
    p.height = 2;
    p.stride[0] = 2;
    p.stride[1] = 1;
    p.stride[2] = 1;
    p.plane[0] = QByteArray(4, char(128));
    p.plane[1] = QByteArray(1, char(64));   // single chroma cell for the 2x2 block
    p.plane[2] = QByteArray(1, char(192));
    FrameHandle h = makeCpuFrameHandle(p, FrameMetadata{});

    ColorMetadata c;
    const CpuPlanes rgba = formatcanon::referenceComposeGridRgba8({h}, 2, 2, c);
    // All four luma pixels share the one chroma cell -> identical RGB (decimation).
    auto px = [&](int x, int y) {
        const int o = y * rgba.stride[0] + x * 4;
        return std::make_tuple(uchar(rgba.plane[0].at(o)), uchar(rgba.plane[0].at(o + 1)),
                               uchar(rgba.plane[0].at(o + 2)));
    };
    QCOMPARE(px(0, 0), px(1, 1));
    QCOMPARE(px(1, 0), px(0, 1));
    QCOMPARE(px(0, 0), px(1, 0));
}

void TestFormatCanon::referenceGridOutputIsRgba8() {
    QList<FrameHandle> frames{solidYuv420pHandle(4, 4, 100, 128, 128)};
    const CpuPlanes rgba =
        formatcanon::referenceComposeGridRgba8(frames, 4, 4, ColorMetadata{});
    QCOMPARE(int(rgba.format), int(FramePixelFormat::Rgba8));
    QCOMPARE(rgba.stride[0], 16); // 4 px * 4 bytes
    QCOMPARE(rgba.plane[0].size(), 4 * 4 * 4);
}
```

Add `#include <tuple>` at the top of the test file for `std::make_tuple`.

- [ ] **Step 2: Run the test to verify it fails**

```sh
cmake --build build/c --target tst_formatcanon
```
Expected: FAIL to compile — `formatcanon::referenceComposeGridRgba8` undeclared.

- [ ] **Step 3: Write the minimal implementation**

Declaration in `formatcanon.h` (add `#include <QList>` and a forward of `FrameHandle` via `framehandle.h` already included; `ColorMetadata` via `colormetadata.h`):

```cpp
#include <QList>
class FrameHandle;

CpuPlanes referenceComposeGridRgba8(const QList<FrameHandle>& frames, int width, int height,
                                    ColorMetadata color);
```

In `formatcanon.cpp` (add `#include "playback/output/framehandle.h"`, `#include <cmath>`):

```cpp
CpuPlanes referenceComposeGridRgba8(const QList<FrameHandle>& frames, int width, int height,
                                    ColorMetadata color) {
    CpuPlanes out;
    out.format = FramePixelFormat::Rgba8;
    out.width = width;
    out.height = height;
    out.stride[0] = width * 4;
    out.plane[0] = QByteArray(width * height * 4, 0);

    // Neutral fill: Y=16, U=V=128 (the CPU compositor's background), converted.
    const Rgb8 bg = yuvToRgb8(16, 128, 128, color.matrix, color.range);
    for (int i = 0; i < width * height; ++i) {
        out.plane[0][4 * i + 0] = char(bg.r);
        out.plane[0][4 * i + 1] = char(bg.g);
        out.plane[0][4 * i + 2] = char(bg.b);
        out.plane[0][4 * i + 3] = char(255);
    }

    const int count = qMax(1, frames.size());
    const int columns = qMax(1, int(std::ceil(std::sqrt(double(count)))));
    const int rows = qMax(1, int(std::ceil(double(count) / double(columns))));

    for (int i = 0; i < frames.size(); ++i) {
        // Pull the source to Yuv420p, then NV12-round-trip so chroma is decimated
        // to 2x2-block resolution (rounding source #1).
        const CpuPlanes planar = frames.at(i).readToCpu(FramePixelFormat::Yuv420p);
        if (!planar.isValid()) continue;
        const CpuPlanes viaNv12 = nv12ToYuv420p(yuv420pToNv12(planar));
        const FullResChroma chroma = upsampleChromaNearest(viaNv12);
        const int srcW = viaNv12.width;
        const int srcH = viaNv12.height;
        if (srcW <= 0 || srcH <= 0) continue;

        const int col = i % columns;
        const int row = i / columns;
        const int dstX = col * width / columns;
        const int dstY = row * height / rows;
        const int dstRight = (col + 1) * width / columns;
        const int dstBottom = (row + 1) * height / rows;
        const int dstW = qMax(0, dstRight - dstX);
        const int dstH = qMax(0, dstBottom - dstY);

        for (int y = 0; y < dstH; ++y) {
            // Nearest-neighbor scale (rounding source #2), identical to scalePlaneNearest.
            const int sy = qMin(srcH - 1, (y * srcH) / dstH);
            for (int x = 0; x < dstW; ++x) {
                const int sx = qMin(srcW - 1, (x * srcW) / dstW);
                const uchar yv = uchar(viaNv12.plane[0].at(sy * viaNv12.stride[0] + sx));
                const uchar uv = uchar(chroma.u.at(sy * srcW + sx));
                const uchar vv = uchar(chroma.v.at(sy * srcW + sx));
                const Rgb8 rgb = yuvToRgb8(yv, uv, vv, color.matrix, color.range);
                const int o = (dstY + y) * out.stride[0] + (dstX + x) * 4;
                out.plane[0][o + 0] = char(rgb.r);
                out.plane[0][o + 1] = char(rgb.g);
                out.plane[0][o + 2] = char(rgb.b);
                out.plane[0][o + 3] = char(255);
            }
        }
    }
    return out;
}
```

- [ ] **Step 4: Run the test to verify it passes**

```sh
cmake --build build/c --target tst_formatcanon && ctest --test-dir build/c -R tst_formatcanon --output-on-failure
```
Expected: PASS (14 test functions).

- [ ] **Step 5: Commit**

```sh
git add playback/output/formatcanon.h playback/output/formatcanon.cpp tests/unit/tst_formatcanon.cpp
git commit -m "feat(format-canon): Rgba8 reference reconstructor (NV12 + nearest-neighbor double rounding)"
```

---

## Task 6: Cross-check the reconstructor's luma placement against the retained CPU oracle

D3/§9: the CPU compositor stays the permanent reference. This task pins that the reconstructor's *grid geometry* (tile placement and nearest-neighbor luma scaling) is byte-identical to `Yuv420pCompositor::composeGrid` — so any future divergence in the reconstructor's placement is caught against the oracle that the existing `tst_yuv420pcompositor` goldens already lock. It converts the CPU oracle's Y plane through the *same* `yuvToRgb8` luma-only path (U=V=128) and asserts equality with the reconstructor for an achromatic input, isolating geometry from color.

**Files:**
- Test: `tests/unit/tst_formatcanon.cpp`
- (No production code change — this is a pure cross-validation test against the retained oracle.)

**Interfaces:**
- Consumes: `Yuv420pCompositor::composeGrid(const QList<FrameHandle>&, int, int)` (keystone-migrated signature), `formatcanon::referenceComposeGridRgba8`, `formatcanon::yuvToRgb8`.

- [ ] **Step 1: Write the failing test**

Add to `tst_formatcanon.cpp`:

```cpp
    void reconstructorLumaPlacementMatchesCpuOracle();
```

```cpp
void TestFormatCanon::reconstructorLumaPlacementMatchesCpuOracle() {
    // Achromatic sources (U=V=128) so chroma decimation is a no-op and only luma
    // GEOMETRY is under test. The reconstructor's per-pixel luma (recovered from
    // its grey RGB) must match the CPU oracle's composed Y plane pixel-for-pixel.
    QList<FrameHandle> frames{solidYuv420pHandle(4, 4, 40, 128, 128),
                              solidYuv420pHandle(4, 4, 80, 128, 128),
                              solidYuv420pHandle(4, 4, 120, 128, 128),
                              solidYuv420pHandle(4, 4, 160, 128, 128)};
    const FrameHandle cpu = Yuv420pCompositor::composeGrid(frames, 8, 8);
    const CpuPlanes cpuPlanes = cpu.readToCpu(FramePixelFormat::Yuv420p);
    QVERIFY(cpuPlanes.isValid());

    ColorMetadata c; // Bt709 / Video
    const CpuPlanes rgba = formatcanon::referenceComposeGridRgba8(frames, 8, 8, c);

    // For each pixel: the oracle's Y (with U=V=128) maps to a grey whose channels
    // are all equal; recompute that grey and compare to the reconstructor's pixel.
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const uchar yv = uchar(cpuPlanes.plane[0].at(y * cpuPlanes.stride[0] + x));
            const formatcanon::Rgb8 grey =
                formatcanon::yuvToRgb8(yv, 128, 128, c.matrix, c.range);
            const int o = y * rgba.stride[0] + x * 4;
            QCOMPARE(uchar(rgba.plane[0].at(o + 0)), grey.r);
            QCOMPARE(uchar(rgba.plane[0].at(o + 1)), grey.g);
            QCOMPARE(uchar(rgba.plane[0].at(o + 2)), grey.b);
        }
    }
}
```

- [ ] **Step 2: Run the test to verify it fails**

If the reconstructor's geometry already agrees, this test would pass on first build — to honor TDD, first introduce it expecting RED only if geometry drifted. Since geometry is shared by construction, run it to confirm and treat a PASS here as the cross-check landing green:

```sh
cmake --build build/c --target tst_formatcanon && ctest --test-dir build/c -R tst_formatcanon --output-on-failure
```
Expected: PASS. If it FAILS, the reconstructor's tile-rect math has diverged from `yuv420pcompositor.cpp:36-42` — fix the reconstructor's `dstX/dstY/dstRight/dstBottom` to match exactly, not the test.

> Rationale for a non-RED-first step here: this is a *cross-validation guard*, not a new behavior. Its value is catching future drift between two already-consistent implementations; the spec (§9) mandates the CPU oracle as the anchor, so the test asserts that anchoring holds.

- [ ] **Step 3: (No implementation change expected)**

If Step 2 passed, proceed. If it failed, the minimal fix is to align the reconstructor's tile-rect expressions in `referenceComposeGridRgba8` with `Yuv420pCompositor::composeGrid` (`yuv420pcompositor.cpp:36-42`) verbatim, then re-run.

- [ ] **Step 4: Run the full unit label (a shared-lib change can affect siblings)**

```sh
ctest --test-dir build/c -L unit --output-on-failure
```
Expected: PASS — all unit tests, including the unchanged `tst_yuv420pcompositor` goldens (the reconstructor adds code, changes no CPU-path value).

- [ ] **Step 5: Commit**

```sh
git add tests/unit/tst_formatcanon.cpp
git commit -m "test(format-canon): cross-check reconstructor luma placement vs CPU oracle"
```

---

## Task 7: Compositor working/output format + per-sink export-conversion contract

`format-canon` owns (spec §6) "the compositor working/output format (`Rgba8`)" and "the per-sink export conversion". The compositor and sinks are GPU-phase work (`gpu-compositor`/`async-readback`, Phase 3–4), but the *contract* — what `Rgba8` working format converts to for each sink — is owned here and pinned now with an integer export kernel + a contract test, so the later GPU export edge has a CPU oracle to match. This task adds:
- `exportRgba8ToYuv420p` (for NDI's `packI420`, `ndisink.cpp:132`) and `exportRgba8ToNv12` (for `gpu-encode`),
- a `sinkExportFormat(OutputTargetKind)` mapping table documenting which CPU format each sink's `readToCpu` requests.

> **Phase ordering:** this is the format *contract*, testable now (pure integer code). Its *enforcement* at real sink call sites (rewiring `ndisink.cpp` / `qtpreviewsink.cpp` to pull `readToCpu(sinkExportFormat(kind))`) is Phase-2/Phase-4 work owned by `gpu-compositor`/`async-readback` and is **not done here** — this task only defines and tests the contract so those subprojects consume a fixed mapping, not a moving one.

**Files:**
- Modify: `playback/output/formatcanon.h`, `playback/output/formatcanon.cpp`
- Test: `tests/unit/tst_formatcanon.cpp`

**Interfaces:**
- Consumes: `OutputTargetKind` from `playback/output/outputtypes.h`; `ColorMetadata`; Task-2/4 kernels.
- Produces:
  ```cpp
  namespace formatcanon {
  // RGB -> YUV is the inverse of yuvToRgb8 (integer Q10, video/full + 601/709).
  // Used only at the export edge (the pipeline interior is YUV; only sinks that
  // need YUV from an Rgba8 compositor surface call these).
  CpuPlanes exportRgba8ToYuv420p(const CpuPlanes& rgba, ColorMetadata color);
  CpuPlanes exportRgba8ToNv12(const CpuPlanes& rgba, ColorMetadata color);
  // The CPU format a given sink's readback requests from an Rgba8 bus surface.
  // NDI -> Yuv420p (packI420); QtPreview -> Rgba8 (QVideoFrame RGBA path);
  // DeckLink/St2110/Omt/Aja -> Yuv420p (CPU-frame SDKs). Encode is on the recorder
  // path (gpu-encode), not an OutputTargetKind, and requests Nv12 directly.
  FramePixelFormat sinkExportFormat(OutputTargetKind kind);
  } // namespace formatcanon
  ```

- [ ] **Step 1: Write the failing test**

Add to `tst_formatcanon.cpp`, including `#include "playback/output/outputtypes.h"`:

```cpp
    void sinkExportFormatMapsEachTarget();
    void rgbToYuvRoundTripsWithinTolerance();
    void exportRgba8ToNv12IsInterleaved();
```

```cpp
void TestFormatCanon::sinkExportFormatMapsEachTarget() {
    using namespace formatcanon;
    QCOMPARE(int(sinkExportFormat(OutputTargetKind::Ndi)), int(FramePixelFormat::Yuv420p));
    QCOMPARE(int(sinkExportFormat(OutputTargetKind::QtPreview)), int(FramePixelFormat::Rgba8));
    QCOMPARE(int(sinkExportFormat(OutputTargetKind::DeckLinkSdiHdmi)),
             int(FramePixelFormat::Yuv420p));
    QCOMPARE(int(sinkExportFormat(OutputTargetKind::DeckLinkIpSt2110)),
             int(FramePixelFormat::Yuv420p));
    QCOMPARE(int(sinkExportFormat(OutputTargetKind::Omt)), int(FramePixelFormat::Yuv420p));
    QCOMPARE(int(sinkExportFormat(OutputTargetKind::Aja)), int(FramePixelFormat::Yuv420p));
}

void TestFormatCanon::rgbToYuvRoundTripsWithinTolerance() {
    // Build a 2x2 Rgba8 from known YUV via yuvToRgb8, export back to Yuv420p, and
    // assert the recovered Y is within +/-2 (8-bit YUV->RGB->YUV rounding).
    ColorMetadata c;
    const formatcanon::Rgb8 src = formatcanon::yuvToRgb8(150, 128, 128, c.matrix, c.range);
    CpuPlanes rgba;
    rgba.format = FramePixelFormat::Rgba8;
    rgba.width = 2;
    rgba.height = 2;
    rgba.stride[0] = 8;
    rgba.plane[0] = QByteArray(2 * 2 * 4, 0);
    for (int i = 0; i < 4; ++i) {
        rgba.plane[0][4 * i + 0] = char(src.r);
        rgba.plane[0][4 * i + 1] = char(src.g);
        rgba.plane[0][4 * i + 2] = char(src.b);
        rgba.plane[0][4 * i + 3] = char(255);
    }
    const CpuPlanes yuv = formatcanon::exportRgba8ToYuv420p(rgba, c);
    QVERIFY(yuv.isValid());
    QCOMPARE(int(yuv.format), int(FramePixelFormat::Yuv420p));
    const int recoveredY = uchar(yuv.plane[0].at(0));
    QVERIFY2(qAbs(recoveredY - 150) <= 2, qPrintable(QString::number(recoveredY)));
}

void TestFormatCanon::exportRgba8ToNv12IsInterleaved() {
    ColorMetadata c;
    const formatcanon::Rgb8 src = formatcanon::yuvToRgb8(120, 100, 150, c.matrix, c.range);
    CpuPlanes rgba;
    rgba.format = FramePixelFormat::Rgba8;
    rgba.width = 2;
    rgba.height = 2;
    rgba.stride[0] = 8;
    rgba.plane[0] = QByteArray(16, 0);
    for (int i = 0; i < 4; ++i) {
        rgba.plane[0][4 * i + 0] = char(src.r);
        rgba.plane[0][4 * i + 1] = char(src.g);
        rgba.plane[0][4 * i + 2] = char(src.b);
        rgba.plane[0][4 * i + 3] = char(255);
    }
    const CpuPlanes nv12 = formatcanon::exportRgba8ToNv12(rgba, c);
    QVERIFY(nv12.isValid());
    QCOMPARE(int(nv12.format), int(FramePixelFormat::Nv12));
    QCOMPARE(nv12.stride[1], 2); // 1 chroma pair -> 2 bytes wide
    // UV interleaved: byte 0 = U, byte 1 = V, both near (100,150).
    QVERIFY(qAbs(int(uchar(nv12.plane[1].at(0))) - 100) <= 2);
    QVERIFY(qAbs(int(uchar(nv12.plane[1].at(1))) - 150) <= 2);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```sh
cmake --build build/c --target tst_formatcanon
```
Expected: FAIL to compile — `formatcanon::sinkExportFormat` / `exportRgba8ToYuv420p` / `exportRgba8ToNv12` undeclared.

- [ ] **Step 3: Write the minimal implementation**

Declarations in `formatcanon.h` (add `#include "playback/output/outputtypes.h"`):

```cpp
CpuPlanes exportRgba8ToYuv420p(const CpuPlanes& rgba, ColorMetadata color);
CpuPlanes exportRgba8ToNv12(const CpuPlanes& rgba, ColorMetadata color);
FramePixelFormat sinkExportFormat(OutputTargetKind kind);
```

In `formatcanon.cpp`:

```cpp
namespace {
// Inverse of yuvToRgb8 (Q10, video/full + 601/709). Subsamples chroma by
// averaging the 2x2 RGB block before converting, matching 4:2:0 export.
void rgbToYuvSample(uchar r, uchar g, uchar b, ColorMatrix matrix, ColorRange range, int* y,
                    int* cb, int* cr) {
    int yy, u, v;
    if (matrix == ColorMatrix::Bt601) {
        yy = 263 * r + 516 * g + 100 * b;          // ~0.257,0.504,0.098 in Q10
        u = -152 * r - 298 * g + 450 * b;           // Cb
        v = 450 * r - 377 * g - 73 * b;             // Cr
    } else {                                        // Bt709
        yy = 187 * r + 629 * g + 63 * b;            // ~0.183,0.614,0.062
        u = -103 * r - 347 * g + 450 * b;
        v = 450 * r - 409 * g - 41 * b;
    }
    if (range == ColorRange::Video) {
        *y = ((yy + 512) >> 10) + 16;
    } else {
        *y = ((yy * 1024 / 876 + 512) >> 10); // rescale limited-luma coeffs to full
    }
    *cb = ((u + 512) >> 10) + 128;
    *cr = ((v + 512) >> 10) + 128;
}
uchar clampU8b(int v) { return uchar(v < 0 ? 0 : (v > 255 ? 255 : v)); }
} // namespace

CpuPlanes exportRgba8ToYuv420p(const CpuPlanes& rgba, ColorMetadata color) {
    if (rgba.format != FramePixelFormat::Rgba8 || !rgba.isValid()) return CpuPlanes{};
    const int w = rgba.width;
    const int h = rgba.height;
    const int cw = (w + 1) / 2;
    const int ch = (h + 1) / 2;

    CpuPlanes out;
    out.format = FramePixelFormat::Yuv420p;
    out.width = w;
    out.height = h;
    out.stride[0] = w;
    out.stride[1] = cw;
    out.stride[2] = cw;
    out.plane[0] = QByteArray(w * h, 0);
    out.plane[1] = QByteArray(cw * ch, char(128));
    out.plane[2] = QByteArray(cw * ch, char(128));

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int o = y * rgba.stride[0] + x * 4;
            int yy, cb, cr;
            rgbToYuvSample(uchar(rgba.plane[0].at(o)), uchar(rgba.plane[0].at(o + 1)),
                           uchar(rgba.plane[0].at(o + 2)), color.matrix, color.range, &yy, &cb,
                           &cr);
            out.plane[0][y * w + x] = char(clampU8b(yy));
            // Chroma: write the top-left pixel of each 2x2 block (point sub-sample).
            if ((x % 2 == 0) && (y % 2 == 0)) {
                out.plane[1][(y / 2) * cw + (x / 2)] = char(clampU8b(cb));
                out.plane[2][(y / 2) * cw + (x / 2)] = char(clampU8b(cr));
            }
        }
    }
    return out;
}

CpuPlanes exportRgba8ToNv12(const CpuPlanes& rgba, ColorMetadata color) {
    const CpuPlanes planar = exportRgba8ToYuv420p(rgba, color);
    if (!planar.isValid()) return CpuPlanes{};
    return yuv420pToNv12(planar);
}

FramePixelFormat sinkExportFormat(OutputTargetKind kind) {
    switch (kind) {
    case OutputTargetKind::QtPreview:
        return FramePixelFormat::Rgba8;
    case OutputTargetKind::Ndi:
    case OutputTargetKind::DeckLinkSdiHdmi:
    case OutputTargetKind::DeckLinkIpSt2110:
    case OutputTargetKind::Omt:
    case OutputTargetKind::Aja:
        return FramePixelFormat::Yuv420p;
    }
    return FramePixelFormat::Yuv420p;
}
```

> The `rgbToYuvSample` coefficients are a *first-pass* integer inverse; the round-trip test tolerates ±2 so they need not be the exact algebraic inverse of `yuvToRgb8`. If `rgbToYuvRoundTripsWithinTolerance` exceeds ±2, tighten the Q10 forward coefficients (they must invert the Task-4 constants); do not widen the tolerance — ±2 is the export-edge contract.

- [ ] **Step 4: Run the test to verify it passes**

```sh
cmake --build build/c --target tst_formatcanon && ctest --test-dir build/c -R tst_formatcanon --output-on-failure
```
Expected: PASS (17 test functions).

- [ ] **Step 5: Run the full unit label + format changed lines**

```sh
ctest --test-dir build/c -L unit --output-on-failure
CF=/opt/homebrew/opt/llvm/bin/clang-format
GCF=/opt/homebrew/opt/llvm/bin/git-clang-format
python3 "$GCF" --binary "$CF" --diff --commit origin/main -- '*.cpp' '*.h'
```
Expected: full unit suite PASS; clang-format reports no changes on the changed lines (the new files are written in the prevailing style). If it reports diffs, stage and apply only changed lines: `python3 "$GCF" --binary "$CF" --commit origin/main -- '*.cpp' '*.h'`.

- [ ] **Step 6: Commit**

```sh
git add playback/output/formatcanon.h playback/output/formatcanon.cpp tests/unit/tst_formatcanon.cpp
git commit -m "feat(format-canon): per-sink export conversion + Rgba8->YUV/NV12 contract"
```

---

## Done criteria (subproject)

- `playback/output/formatcanon.{h,cpp}` provide: plane-shape geometry, NV12↔Yuv420p (de)interleave, nearest 2×2-block chroma upsample, integer BT.601/709 + range YUV→Rgba8, the `referenceComposeGridRgba8` golden oracle (NV12 + nearest-neighbor double rounding), and the Rgba8→YUV/NV12 export + `sinkExportFormat` contract.
- `tst_formatcanon` (17 functions) is green under `ctest -L unit`; the existing `tst_yuv420pcompositor` goldens are unchanged (CPU-path zero-regression).
- The reconstructor's grid placement is cross-checked against the retained CPU oracle (Task 6).
- No GPU/RHI code; everything testable today. The reconstructor is the permanent oracle `gpu-compositor` will validate the compat shader against — exact on WARP, ±1 LSB on a local-GPU lane (handed off to `gpu-compositor`).
- The integer YUV→RGB constants (Task 4) and the tile-rect geometry (Task 5) are the documented contract the GPU compat shader must reproduce verbatim.

## Probe contingencies (Phase 0, §7.6 / D2)

- **Color-tag audit (P0.6):** Tasks 4–5 default `ColorMetadata` to `Bt709 / Video`. If the Phase-0 fixture audit finds untagged goldens with a *different* effective default (the today `height>576 → BT709/601` heuristic), the reconstructor's default `ColorMetadata` argument must be supplied by `color-metadata`'s default-tagging policy, not hard-coded here. The reconstructor already takes `ColorMetadata` as a parameter, so this is a caller concern — no `format-canon` change needed, but note it when `color-metadata` lands.
- **NV12 chroma siting:** Task 3 models chroma as co-sited at the top-left of each 2×2 luma block (`(x>>1, y>>1)`). If the Phase-0 probe reveals the VT/MF decoder emits MPEG-2-style center-sited chroma that the GPU sampler reproduces differently, the `upsampleChromaNearest` siting must match the shader; the ±1 LSB local-GPU lane absorbs sub-sample siting, but the WARP-exact lane requires the reconstructor and shader to agree on siting. Revisit when `gpu-compositor`'s shader is authored.
