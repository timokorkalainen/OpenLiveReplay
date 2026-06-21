# FrameHandle Keystone Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (- [ ]) syntax.

**Goal:** Replace the value-typed `MediaVideoFrame` with an opaque, ref-counted, immutable `FrameHandle` (a `shared_ptr<const IFrameData>` for pixels plus a cheap per-handle `FrameMetadata` override) that separates payload identity from presentation metadata, ships CPU-backed only, and migrates every existing access site behind the new API with **byte-for-byte identical** assertion values and golden outputs.

**Architecture:** A new `playback/output/framehandle.*` family defines `FramePixelFormat`, `ColorMetadata`, `FramePayloadKey`, `FrameMetadata`, `IFrameData`, a `CpuFrameData` concrete (today's three `QByteArray` planes), `FrameHandle` (value wrapper: copy = refcount bump of shared pixels + cheap metadata copy), and a `MediaVideoFrameView` compat value-view backed by `readToCpu(Yuv420p)` so plane-indexing test/product sites migrate mechanically. `OutputFrameCache`, `TrackBuffer`, the inactive-graph snapshot, `OutputBusFrame.video`, `MultiviewComposite.video`, the compositor and all sinks store/consume handle **refs** (D9), never second copies.

**Tech Stack:** C++17, Qt 6 (Core/Test), FFmpeg (`libav*` for `AVFrame` ingest at the decode edge only), CMake + Ninja. CPU-backed only this phase — no RHI, no GPU surfaces, no native frameworks.

## Global Constraints

- **Keystone-first.** This subproject defines the canonical public interfaces every downstream GPU subproject (`format-canon`, `gpu-abstraction`, `gpu-compositor`, `async-readback`, …) consumes. Name every type and signature exactly as written here; downstream plans depend on these names verbatim.
- **CPU path stays default + reference.** Phase 1 ships **CPU-backed only**. `IFrameData::isGpuBacked()` is always `false`; `gpuSurface()` always returns `nullptr`; `readToCpu()` is a no-op format-passthrough (or a CPU NV12↔I420 convert) — there is no GPU code in this phase. The CPU pipeline remains the permanent correctness oracle and runtime fallback.
- **Everything behind flags / additive.** No behavior change. The handle is a representation swap; product output (pixels, identity, dedup, AV-sync, goldens) is unchanged. No new runtime flag is needed in Phase 1 because nothing branches on GPU residency yet — `OLR_GPU_PIPELINE` arrives with `gpu-abstraction` (Phase 2).
- **No throwaways.** Every artifact is production and stays in the tree. The `MediaVideoFrameView` compat view is permanent (it is the plane-indexing read path), not a migration scaffold to delete later.
- **Public-repo professionalism.** This repo is public. Code, comments, and commit messages must be self-contained and professional: document the present design, no internal notes, no references to private history.
- **Format changed lines only.** CI's Lint job checks clang-format on changed lines only; several engine files use hand-written Allman style. Format only the lines you change:
  ```sh
  CF=/opt/homebrew/opt/llvm/bin/clang-format
  GCF=/opt/homebrew/opt/llvm/bin/git-clang-format
  python3 "$GCF" --binary "$CF" --diff --commit origin/main -- '*.cpp' '*.h'
  ```
- **The zero-regression gate (the whole point).** After every task, the entire existing unit suite (`ctest -L unit`) and `e2e_play` suite must pass with **identical assertion values and golden outputs**. Test *sources* that index `.planeY/.planeU/.planeV` are mechanically rewritten through the compat view — that is expected churn, not a regression. **Product behavior, identity, dedup, and goldens are unchanged.**
- **Build (run from the worktree root):** configure once:
  ```sh
  cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON
  cmake --build build/c --target <target>
  ctest --test-dir build/c -L unit --output-on-failure
  ```
  Unit tests register via `olr_add_unit_test(<name> olr_test_playback)` in `tests/unit/CMakeLists.txt`; Qt Test runs headless under `QT_QPA_PLATFORM=offscreen`.

---

## File Structure

- **Create** `playback/output/framepixelformat.h` — `FramePixelFormat` enum + `planeCount()` accessor.
- **Create** `playback/output/colormetadata.h` — `ColorMetadata` (matrix/range/primaries/transfer/chromaFormat/bitDepth).
- **Create** `playback/output/framehandle.h`, `playback/output/framehandle.cpp` — `CpuPlanes`, `FramePayloadKey`, `FrameMetadata`, `IFrameData`, `CpuFrameData`, `FrameHandle`, `MediaVideoFrameView`, plus `makeCpuFrameHandle(...)` and `solidYuv420pHandle(...)` factories.
- **Modify** `playback/output/outputtypes.h` — leave `MediaPixelFormat` as-is (legacy), no edit needed unless noted.
- **Modify** `playback/output/outputframecache.h/.cpp` — store/return `FrameHandle` (refs, D9).
- **Modify** `playback/trackbuffer.h/.cpp` — `TrackBuffer::Frame.frame` becomes a `FrameHandle`.
- **Modify** `playback/output/outputbusengine.h/.cpp` — `OutputBusFrame.video` / `MultiviewComposite.video` become `FrameHandle`; metadata override replaces `ptsMs`/`outputFrameIndex` mutation.
- **Modify** `playback/output/yuv420pcompositor.h/.cpp` — `composeGrid` takes/returns handles.
- **Modify** `playback/output/ndisink.cpp`, `playback/output/qtpreviewsink.cpp` — read via the compat view.
- **Modify** `playback/playbackworker.cpp/.h` — `convertToMediaVideoFrame` returns a `FrameHandle`; cache/buffer inserts share refs; inactive-graph snapshot aliases refs.
- **Create** tests: `tests/unit/tst_framehandle.cpp`; mechanically migrate `tst_outputbusengine.cpp`, `tst_outputframecache.cpp`, `tst_trackbuffer.cpp`, `tst_yuv420pcompositor.cpp`, `tst_ndisink.cpp`, `tst_outputdispatcher.cpp`, `tst_outputdispatcher_holdlast.cpp`, `tst_outputruntime.cpp`, `tst_sharedcacheslot.cpp` through the compat view.
- **Modify** `tests/unit/CMakeLists.txt`, `CMakeLists.txt` — register `tst_framehandle`; add `framehandle.cpp` to the engine source list.

---

## Task 1: `FramePixelFormat` enum + plane-count accessor

**Files:**
- Create: `playback/output/framepixelformat.h`
- Test: `tests/unit/tst_framehandle.cpp`
- Modify: `tests/unit/CMakeLists.txt`

**Interfaces:**
- Produces:
  ```cpp
  enum class FramePixelFormat { Nv12, Yuv420p, Rgba8 };
  // Number of distinct memory planes for a format: Nv12 → 2 (Y + interleaved UV),
  // Yuv420p → 3 (Y, U, V), Rgba8 → 1 (packed).
  constexpr int planeCount(FramePixelFormat format);
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_framehandle.cpp`:

```cpp
#include <QtTest>

#include "playback/output/framepixelformat.h"

class TestFrameHandle : public QObject {
    Q_OBJECT
private slots:
    void planeCountPerFormat();
};

void TestFrameHandle::planeCountPerFormat() {
    QCOMPARE(planeCount(FramePixelFormat::Nv12), 2);
    QCOMPARE(planeCount(FramePixelFormat::Yuv420p), 3);
    QCOMPARE(planeCount(FramePixelFormat::Rgba8), 1);
}

QTEST_GUILESS_MAIN(TestFrameHandle)
#include "tst_framehandle.moc"
```

Register in `tests/unit/CMakeLists.txt` immediately after the `tst_outputbusengine` line (line 92):

```cmake
olr_add_unit_test(tst_framehandle olr_test_playback)
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_framehandle
```
Expected: FAIL to compile — `playback/output/framepixelformat.h: No such file or directory`.

- [ ] **Step 3: Write the minimal implementation**

Create `playback/output/framepixelformat.h`:

```cpp
#ifndef FRAMEPIXELFORMAT_H
#define FRAMEPIXELFORMAT_H

// The canonical pixel-format set the GPU-resident pipeline carries on every
// FrameHandle. Phase 1 only ever produces Yuv420p (today's CPU layout); Nv12
// (2-plane biplanar, the GPU-resident default) and Rgba8 (compositor working
// format) are declared now so the handle's format field is forward-compatible
// and downstream subprojects share one enum. format-canon owns the eventual
// shader/encoder agreement on these values.
enum class FramePixelFormat {
    Nv12,     // 2-plane biplanar: Y plane + interleaved CbCr plane
    Yuv420p,  // 3-plane planar: Y, U, V
    Rgba8,    // 1-plane packed RGBA, 8 bits/channel
};

// Number of distinct memory planes a format occupies.
constexpr int planeCount(FramePixelFormat format) {
    switch (format) {
        case FramePixelFormat::Nv12:
            return 2;
        case FramePixelFormat::Yuv420p:
            return 3;
        case FramePixelFormat::Rgba8:
            return 1;
    }
    return 0;
}

#endif // FRAMEPIXELFORMAT_H
```

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_framehandle && ctest --test-dir build/c -R tst_framehandle --output-on-failure
```
Expected: PASS (1 test).

- [ ] **Step 5: Commit**

```sh
git add playback/output/framepixelformat.h tests/unit/tst_framehandle.cpp tests/unit/CMakeLists.txt
git commit -m "feat(frame-handle): FramePixelFormat enum + plane-count accessor"
```

---

## Task 2: `ColorMetadata`

**Files:**
- Create: `playback/output/colormetadata.h`
- Test: `tests/unit/tst_framehandle.cpp` (add a slot)

**Interfaces:**
- Produces:
  ```cpp
  enum class ColorMatrix { Bt601, Bt709, Bt2020 };
  enum class ColorRange { Video, Full };
  enum class ColorPrimaries { Bt601, Bt709, Bt2020, Unspecified };
  enum class ColorTransfer { Bt601, Bt709, Bt2020, Unspecified };
  enum class ChromaFormat { Yuv420, Yuv422, Yuv444, Rgb };
  struct ColorMetadata {
      ColorMatrix matrix = ColorMatrix::Bt709;
      ColorRange range = ColorRange::Video;
      ColorPrimaries primaries = ColorPrimaries::Bt709;
      ColorTransfer transfer = ColorTransfer::Bt709;
      ChromaFormat chromaFormat = ChromaFormat::Yuv420;
      int bitDepth = 8;  // fixed at 8 this program (D2 / §1 non-goals)
      bool operator==(const ColorMetadata& o) const;
      bool operator!=(const ColorMetadata& o) const;
  };
  ```

- [ ] **Step 1: Write the failing test**

Add to `tests/unit/tst_framehandle.cpp` — a new slot declaration and body:

```cpp
    void colorMetadataDefaultsAndEquality();
```

```cpp
void TestFrameHandle::colorMetadataDefaultsAndEquality() {
    ColorMetadata a;
    QCOMPARE(a.matrix, ColorMatrix::Bt709);
    QCOMPARE(a.range, ColorRange::Video);
    QCOMPARE(a.primaries, ColorPrimaries::Bt709);
    QCOMPARE(a.transfer, ColorTransfer::Bt709);
    QCOMPARE(a.chromaFormat, ChromaFormat::Yuv420);
    QCOMPARE(a.bitDepth, 8);

    ColorMetadata b;
    QVERIFY(a == b);
    b.matrix = ColorMatrix::Bt601;
    QVERIFY(a != b);
}
```

Add the include at the top of the file under the existing one:

```cpp
#include "playback/output/colormetadata.h"
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_framehandle
```
Expected: FAIL to compile — `playback/output/colormetadata.h: No such file or directory`.

- [ ] **Step 3: Write the implementation**

Create `playback/output/colormetadata.h`:

```cpp
#ifndef COLORMETADATA_H
#define COLORMETADATA_H

// Per-frame color description carried on every FrameHandle. Today the pipeline
// infers color space from frame height (height > 576 -> BT.709, else BT.601;
// video range; see qtpreviewsink.cpp:23). ColorMetadata is the seam that later
// replaces that heuristic with real decoder-extracted tags (the color-metadata
// subproject). Phase 1 only constructs it with the heuristic defaults, so
// goldens are unchanged.
//
// bitDepth is fixed at 8 for this program (D2 keeps NV12; a 10-bit/HDR path is
// a future program). It is carried for forward-compatibility only.

enum class ColorMatrix { Bt601, Bt709, Bt2020 };
enum class ColorRange { Video, Full };
enum class ColorPrimaries { Bt601, Bt709, Bt2020, Unspecified };
enum class ColorTransfer { Bt601, Bt709, Bt2020, Unspecified };
enum class ChromaFormat { Yuv420, Yuv422, Yuv444, Rgb };

struct ColorMetadata {
    ColorMatrix matrix = ColorMatrix::Bt709;
    ColorRange range = ColorRange::Video;
    ColorPrimaries primaries = ColorPrimaries::Bt709;
    ColorTransfer transfer = ColorTransfer::Bt709;
    ChromaFormat chromaFormat = ChromaFormat::Yuv420;
    int bitDepth = 8;

    bool operator==(const ColorMetadata& o) const {
        return matrix == o.matrix && range == o.range && primaries == o.primaries &&
               transfer == o.transfer && chromaFormat == o.chromaFormat && bitDepth == o.bitDepth;
    }
    bool operator!=(const ColorMetadata& o) const { return !(*this == o); }
};

#endif // COLORMETADATA_H
```

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_framehandle && ctest --test-dir build/c -R tst_framehandle --output-on-failure
```
Expected: PASS (2 tests).

- [ ] **Step 5: Commit**

```sh
git add playback/output/colormetadata.h tests/unit/tst_framehandle.cpp
git commit -m "feat(frame-handle): ColorMetadata (matrix/range/primaries/transfer/chroma, 8-bit fixed)"
```

---

## Task 3: `FramePayloadKey` + `FrameMetadata` (identity split)

**Files:**
- Create: `playback/output/framehandle.h` (this task adds the metadata types; the handle class arrives in Task 4)
- Test: `tests/unit/tst_framehandle.cpp` (add slots)

**Interfaces:**
- Consumes: `FramePixelFormat` (Task 1), `ColorMetadata` (Task 2).
- Produces:
  ```cpp
  // Identity / dedup subset: what makes two frames "the same picture". Mirrors
  // OutputFrameIdentity::samePayloadAs (outputbusengine.h:20) — it EXCLUDES the
  // presentation fields outputFrameIndex / sampledPlayheadMs on purpose, so
  // dedup/identity-skip never regress.
  struct FramePayloadKey {
      int feedIndex = -1;
      qint64 ptsMs = 0;
      quint32 videoHash = 0;
      FramePixelFormat format = FramePixelFormat::Yuv420p;
      int width = 0;
      int height = 0;
      bool isPlaceholder = false;
      bool samePayloadAs(const FramePayloadKey& o) const;
  };

  // FramePayloadKey + presentation fields + per-plane strides + ColorMetadata.
  // This is the cheap per-handle override over shared immutable pixels.
  struct FrameMetadata {
      FramePayloadKey key;
      qint64 outputFrameIndex = -1;   // presentation, NOT identity
      qint64 sampledPlayheadMs = 0;   // presentation, NOT identity
      int stride[3] = {0, 0, 0};
      ColorMetadata color;
  };
  ```

> Implementation note: to keep call sites terse without reference-member copy hazards, `FrameMetadata` exposes the key fields via plain accessor inline methods rather than reference members. The concrete header below is authoritative.

- [ ] **Step 1: Write the failing test**

Add to `tests/unit/tst_framehandle.cpp`:

```cpp
    void payloadKeyExcludesPresentationFields();
    void frameMetadataCarriesPresentationAndColor();
```

```cpp
void TestFrameHandle::payloadKeyExcludesPresentationFields() {
    FramePayloadKey a;
    a.feedIndex = 1;
    a.ptsMs = 100;
    a.videoHash = 0xABCD;
    a.format = FramePixelFormat::Yuv420p;
    a.width = 4;
    a.height = 4;
    a.isPlaceholder = false;

    FramePayloadKey b = a;
    QVERIFY(a.samePayloadAs(b));

    // Changing identity fields breaks payload equality.
    b.ptsMs = 101;
    QVERIFY(!a.samePayloadAs(b));
    b = a;
    b.videoHash = 0x1234;
    QVERIFY(!a.samePayloadAs(b));
    b = a;
    b.feedIndex = 2;
    QVERIFY(!a.samePayloadAs(b));
}

void TestFrameHandle::frameMetadataCarriesPresentationAndColor() {
    FrameMetadata m;
    m.key.feedIndex = 3;
    m.key.ptsMs = 200;
    m.key.format = FramePixelFormat::Yuv420p;
    m.key.width = 8;
    m.key.height = 8;
    m.outputFrameIndex = 42;
    m.sampledPlayheadMs = 333;
    m.stride[0] = 8;
    m.stride[1] = 4;
    m.stride[2] = 4;
    m.color.matrix = ColorMatrix::Bt601;

    QCOMPARE(m.key.feedIndex, 3);
    QCOMPARE(m.outputFrameIndex, qint64(42));
    QCOMPARE(m.sampledPlayheadMs, qint64(333));
    QCOMPARE(m.stride[1], 4);
    QCOMPARE(m.color.matrix, ColorMatrix::Bt601);

    // Presentation fields are NOT part of payload identity: two metadata with
    // identical keys but different presentation fields share one payload key.
    FrameMetadata n = m;
    n.outputFrameIndex = 99;
    n.sampledPlayheadMs = 777;
    QVERIFY(m.key.samePayloadAs(n.key));
}
```

Add include at top of file:

```cpp
#include "playback/output/framehandle.h"
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_framehandle
```
Expected: FAIL to compile — `playback/output/framehandle.h: No such file or directory`.

- [ ] **Step 3: Write the implementation**

Create `playback/output/framehandle.h` (metadata-only for now; the handle class is added in Task 4):

```cpp
#ifndef FRAMEHANDLE_H
#define FRAMEHANDLE_H

#include "playback/output/colormetadata.h"
#include "playback/output/framepixelformat.h"

#include <QtGlobal>

// Payload identity / dedup subset of a frame's metadata: the fields that make
// two frames "the same picture". This deliberately EXCLUDES the presentation
// fields outputFrameIndex / sampledPlayheadMs, mirroring
// OutputFrameIdentity::samePayloadAs (playback/output/outputbusengine.h:20). If
// identity included outputFrameIndex, the dispatcher's identity-skip would
// regress (it skips submit() on samePayloadAs while presentation advances).
struct FramePayloadKey {
    int feedIndex = -1;
    qint64 ptsMs = 0;
    quint32 videoHash = 0;
    FramePixelFormat format = FramePixelFormat::Yuv420p;
    int width = 0;
    int height = 0;
    bool isPlaceholder = false;

    bool samePayloadAs(const FramePayloadKey& o) const {
        return feedIndex == o.feedIndex && ptsMs == o.ptsMs && videoHash == o.videoHash &&
               format == o.format && width == o.width && height == o.height &&
               isPlaceholder == o.isPlaceholder;
    }
};

// The cheap PER-HANDLE override carried alongside shared immutable pixels:
// the payload key plus presentation metadata (outputFrameIndex /
// sampledPlayheadMs), per-plane strides, and ColorMetadata. Today
// OutputBusEngine mutates ptsMs / outputFrameIndex on a COW-shared
// MediaVideoFrame after sharing it (outputbusengine.cpp:154-155, :181); an
// immutable pixel payload cannot allow that, so those scalars live here, on a
// per-handle value that copies without touching the pixels.
struct FrameMetadata {
    FramePayloadKey key;
    qint64 outputFrameIndex = -1;
    qint64 sampledPlayheadMs = 0;
    int stride[3] = {0, 0, 0};
    ColorMetadata color;
};

#endif // FRAMEHANDLE_H
```

Add `framehandle.h` to the engine source list in `CMakeLists.txt`, immediately after the `mediaframe.h` line (line 193):

```cmake
        playback/output/framepixelformat.h playback/output/colormetadata.h
        playback/output/framehandle.h playback/output/framehandle.cpp
```

> `framehandle.cpp` does not exist yet; create an empty stub now so CMake configures (Task 4 fills it):
> ```sh
> printf '#include "playback/output/framehandle.h"\n' > playback/output/framehandle.cpp
> ```

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake -S . -B build/c -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos -DOLR_BUILD_TESTS=ON
cmake --build build/c --target tst_framehandle && ctest --test-dir build/c -R tst_framehandle --output-on-failure
```
Expected: PASS (4 tests). (Reconfigure is needed because a new source was added to the engine library.)

- [ ] **Step 5: Commit**

```sh
git add playback/output/framehandle.h playback/output/framehandle.cpp tests/unit/tst_framehandle.cpp CMakeLists.txt
git commit -m "feat(frame-handle): FramePayloadKey + FrameMetadata identity/presentation split"
```

---

## Task 4: `IFrameData` + `CpuFrameData` + `FrameHandle` + factories

**Files:**
- Modify: `playback/output/framehandle.h`, `playback/output/framehandle.cpp`
- Test: `tests/unit/tst_framehandle.cpp` (add slots)

**Interfaces:**
- Consumes: `FrameMetadata`, `FramePixelFormat` (Task 3).
- Produces:
  ```cpp
  struct CpuPlanes {
      FramePixelFormat format = FramePixelFormat::Yuv420p;
      int width = 0, height = 0;
      QByteArray plane[3];     // plane[2] unused for Nv12; plane[1..2] unused for Rgba8
      int stride[3] = {0,0,0};
      bool isValid() const;
  };
  class GpuSurface;            // opaque fwd-decl; always nullptr in Phase 1
  class IFrameData {
  public:
      virtual ~IFrameData();
      virtual bool isGpuBacked() const = 0;
      bool isCpuBacked() const { return !isGpuBacked(); }
      virtual CpuPlanes readToCpu(FramePixelFormat target) const = 0;
      virtual GpuSurface* gpuSurface() const = 0;   // null on CPU frames
      virtual FramePixelFormat nativeFormat() const = 0;
  };
  class FrameHandle {
  public:
      FrameHandle();  // null handle (no pixels, default metadata)
      FrameHandle(std::shared_ptr<const IFrameData> data, FrameMetadata meta);
      bool isNull() const;
      const FrameMetadata& metadata() const;
      FrameMetadata& metadata();              // per-handle mutable override
      const IFrameData* data() const;
      std::shared_ptr<const IFrameData> dataPtr() const;  // for ref-sharing/refcount checks
      CpuPlanes readToCpu(FramePixelFormat target = FramePixelFormat::Yuv420p) const;
      bool isGpuBacked() const;
      bool isValid() const;                   // pixels present AND geometry valid
  };
  FrameHandle makeCpuFrameHandle(CpuPlanes planes, FrameMetadata meta);
  FrameHandle solidYuv420pHandle(int width, int height, uchar y, uchar u, uchar v);
  ```

> **Aliasing contract (the keystone invariant):** copying a `FrameHandle` bumps the shared `shared_ptr<const IFrameData>` refcount and copies the cheap `FrameMetadata` value. Mutating `metadata()` on a copy never touches the original's metadata and never touches the shared pixels. This is what makes "override `outputFrameIndex` on a memo-aliased handle, pixels stay byte-identical" hold.

- [ ] **Step 1: Write the failing test (incl. the aliasing/immutability gate)**

Add to `tests/unit/tst_framehandle.cpp`:

```cpp
    void cpuFrameHandleReadToCpuYuv420pIsPassthrough();
    void copyIsRefcountBumpNotPixelCopy();
    void aliasedHandleMetadataOverrideKeepsPixelsByteIdentical();
    void solidHelperMatchesMediaFrameLayout();
    void cpuHandleIsNotGpuBackedAndHasNoSurface();
```

```cpp
void TestFrameHandle::cpuFrameHandleReadToCpuYuv420pIsPassthrough() {
    CpuPlanes p;
    p.format = FramePixelFormat::Yuv420p;
    p.width = 4;
    p.height = 4;
    p.stride[0] = 4; p.stride[1] = 2; p.stride[2] = 2;
    p.plane[0] = QByteArray(16, char(10));
    p.plane[1] = QByteArray(4, char(128));
    p.plane[2] = QByteArray(4, char(128));

    FrameMetadata m;
    m.key.format = FramePixelFormat::Yuv420p;
    m.key.width = 4; m.key.height = 4;
    FrameHandle h = makeCpuFrameHandle(p, m);

    const CpuPlanes out = h.readToCpu(FramePixelFormat::Yuv420p);
    QCOMPARE(out.format, FramePixelFormat::Yuv420p);
    QCOMPARE(out.width, 4);
    QCOMPARE(out.plane[0], p.plane[0]);
    QCOMPARE(out.plane[1], p.plane[1]);
    QCOMPARE(out.plane[2], p.plane[2]);
    // No-op for a CPU-origin frame already in target: the QByteArray shares the
    // same underlying data (COW), not a deep copy.
    QVERIFY(out.plane[0].constData() == p.plane[0].constData());
}

void TestFrameHandle::cpuHandleIsNotGpuBackedAndHasNoSurface() {
    FrameHandle h = solidYuv420pHandle(4, 4, 16, 128, 128);
    QVERIFY(!h.isGpuBacked());
    QVERIFY(h.data()->isCpuBacked());
    QVERIFY(h.data()->gpuSurface() == nullptr);
}

void TestFrameHandle::copyIsRefcountBumpNotPixelCopy() {
    FrameHandle a = solidYuv420pHandle(4, 4, 16, 128, 128);
    const long before = a.dataPtr().use_count();
    FrameHandle b = a;  // copy = refcount bump
    QCOMPARE(b.dataPtr().use_count(), before + 1);
    // Same pixel object, not a clone.
    QVERIFY(a.dataPtr().get() == b.dataPtr().get());
}

void TestFrameHandle::aliasedHandleMetadataOverrideKeepsPixelsByteIdentical() {
    // The keystone copy/equality contract: take a memo-aliased handle, override
    // ptsMs/outputFrameIndex on the copy, assert (a) pixels byte-identical AND
    // refcount-shared with the original, (b) original metadata unchanged.
    FrameHandle original = solidYuv420pHandle(4, 4, 70, 90, 110);
    original.metadata().key.ptsMs = 100;
    original.metadata().outputFrameIndex = 5;

    const CpuPlanes originalPixels = original.readToCpu(FramePixelFormat::Yuv420p);
    const long originalUse = original.dataPtr().use_count();

    FrameHandle aliased = original;  // memo-alias: shares pixels
    aliased.metadata().key.ptsMs = 999;
    aliased.metadata().outputFrameIndex = 42;

    // (a) pixels byte-identical and refcount-shared
    const CpuPlanes aliasedPixels = aliased.readToCpu(FramePixelFormat::Yuv420p);
    QCOMPARE(aliasedPixels.plane[0], originalPixels.plane[0]);
    QCOMPARE(aliasedPixels.plane[1], originalPixels.plane[1]);
    QCOMPARE(aliasedPixels.plane[2], originalPixels.plane[2]);
    QVERIFY(aliased.dataPtr().get() == original.dataPtr().get());
    QCOMPARE(aliased.dataPtr().use_count(), originalUse + 1);

    // (b) original metadata unchanged by the override on the alias
    QCOMPARE(original.metadata().key.ptsMs, qint64(100));
    QCOMPARE(original.metadata().outputFrameIndex, qint64(5));
}

void TestFrameHandle::solidHelperMatchesMediaFrameLayout() {
    // solidYuv420pHandle must reproduce MediaVideoFrame::solidYuv420p exactly so
    // migrated goldens are byte-identical.
    FrameHandle h = solidYuv420pHandle(4, 4, 16, 128, 128);
    const CpuPlanes p = h.readToCpu(FramePixelFormat::Yuv420p);
    QCOMPARE(p.width, 4);
    QCOMPARE(p.height, 4);
    QCOMPARE(p.stride[0], 4);
    QCOMPARE(p.stride[1], 2);
    QCOMPARE(p.stride[2], 2);
    QCOMPARE(p.plane[0], QByteArray(16, char(16)));
    QCOMPARE(p.plane[1], QByteArray(4, char(128)));
    QCOMPARE(p.plane[2], QByteArray(4, char(128)));
}
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_framehandle
```
Expected: FAIL to compile — `makeCpuFrameHandle`, `solidYuv420pHandle`, `CpuPlanes`, `FrameHandle` undeclared.

- [ ] **Step 3: Write the implementation**

Append to `playback/output/framehandle.h` (before `#endif`):

```cpp
#include <QByteArray>

#include <memory>

// A CPU-resident plane set in a specific FramePixelFormat. For Yuv420p all
// three planes are used; for Nv12 plane[0]=Y, plane[1]=interleaved CbCr,
// plane[2] empty; for Rgba8 plane[0] packed, plane[1..2] empty.
struct CpuPlanes {
    FramePixelFormat format = FramePixelFormat::Yuv420p;
    int width = 0;
    int height = 0;
    QByteArray plane[3];
    int stride[3] = {0, 0, 0};

    bool isValid() const {
        if (width <= 0 || height <= 0) return false;
        if (format == FramePixelFormat::Yuv420p)
            return !plane[0].isEmpty() && !plane[1].isEmpty() && !plane[2].isEmpty();
        if (format == FramePixelFormat::Nv12) return !plane[0].isEmpty() && !plane[1].isEmpty();
        return !plane[0].isEmpty();  // Rgba8
    }
};

// Opaque GPU surface handle. No GPU backend exists in Phase 1; the type is a
// forward declaration so the interface is platform-symmetric and downstream
// (gpu-abstraction) defines the concrete. CPU frames always return nullptr.
class GpuSurface;

// The pixel-residency interface every FrameHandle wraps. Phase 1 only ever
// holds a CpuFrameData; isGpuBacked() is always false. readToCpu() is the
// single chokepoint every CPU sink funnels through (a no-op for a CPU-origin
// frame already in target; a download+convert for a future GPU frame).
class IFrameData {
public:
    virtual ~IFrameData();
    virtual bool isGpuBacked() const = 0;
    bool isCpuBacked() const { return !isGpuBacked(); }
    virtual CpuPlanes readToCpu(FramePixelFormat target) const = 0;
    virtual GpuSurface* gpuSurface() const = 0;
    virtual FramePixelFormat nativeFormat() const = 0;
};

// CPU-resident pixel payload: the immutable plane set. Phase 1's only concrete.
// readToCpu(target) is a no-op (COW share) when target == nativeFormat,
// otherwise an in-RAM conversion (Yuv420p<->Nv12).
class CpuFrameData : public IFrameData {
public:
    explicit CpuFrameData(CpuPlanes planes);
    bool isGpuBacked() const override { return false; }
    CpuPlanes readToCpu(FramePixelFormat target) const override;
    GpuSurface* gpuSurface() const override { return nullptr; }
    FramePixelFormat nativeFormat() const override { return m_planes.format; }

private:
    CpuPlanes m_planes;
};

// Value wrapper over shared_ptr<const IFrameData> (immutable pixels) plus a
// cheap per-handle FrameMetadata. Copy = refcount bump of the pixels + a cheap
// metadata value copy. Mutating metadata() never touches the shared pixels.
class FrameHandle {
public:
    FrameHandle() = default;
    FrameHandle(std::shared_ptr<const IFrameData> data, FrameMetadata meta)
        : m_data(std::move(data)), m_meta(std::move(meta)) {}

    bool isNull() const { return m_data == nullptr; }
    const FrameMetadata& metadata() const { return m_meta; }
    FrameMetadata& metadata() { return m_meta; }
    const IFrameData* data() const { return m_data.get(); }
    std::shared_ptr<const IFrameData> dataPtr() const { return m_data; }

    CpuPlanes readToCpu(FramePixelFormat target = FramePixelFormat::Yuv420p) const {
        return m_data ? m_data->readToCpu(target) : CpuPlanes{};
    }
    bool isGpuBacked() const { return m_data && m_data->isGpuBacked(); }
    // A handle is "presentable" when it carries real (non-placeholder) pixels
    // in a known geometry — the FrameHandle analogue of MediaVideoFrame::isValid().
    bool isPresentable() const {
        return m_data && m_meta.key.width > 0 && m_meta.key.height > 0 &&
               readToCpu(FramePixelFormat::Yuv420p).isValid();
    }

private:
    std::shared_ptr<const IFrameData> m_data;
    FrameMetadata m_meta;
};

// Factory: wrap a CPU plane set + metadata into a CPU-backed handle.
FrameHandle makeCpuFrameHandle(CpuPlanes planes, FrameMetadata meta);

// Factory: a solid Yuv420p handle, byte-identical to
// MediaVideoFrame::solidYuv420p(width, height, y, u, v) — used by tests and the
// placeholder/fill paths.
FrameHandle solidYuv420pHandle(int width, int height, uchar y, uchar u, uchar v);
```

> Migrated call sites that previously read `frame.video.isValid()` (e.g. `ndisink.cpp`, `qtpreviewsink.cpp`) route through `FrameHandle::isPresentable()`; sites that go through the compat value view read `MediaVideoFrameView::isValid()` (Task 5) on a `MediaVideoFrameView` constructed from the handle.

Write `playback/output/framehandle.cpp`:

```cpp
#include "playback/output/framehandle.h"

#include <cstring>

IFrameData::~IFrameData() = default;

namespace {

// Deinterleave an Nv12 CbCr plane into separate U and V planes (or interleave
// back). Phase 1 only ever stores Yuv420p, so these conversions exist for the
// readToCpu(Nv12)/readToCpu(Yuv420p) cross requests a future GPU sink may make;
// the Yuv420p->Yuv420p path (the only one hit this phase) is a COW passthrough.
CpuPlanes convertYuv420pToNv12(const CpuPlanes& in) {
    CpuPlanes out;
    out.format = FramePixelFormat::Nv12;
    out.width = in.width;
    out.height = in.height;
    const int chromaW = (in.width + 1) / 2;
    const int chromaH = (in.height + 1) / 2;
    out.stride[0] = in.width;
    out.stride[1] = chromaW * 2;
    out.plane[0] = in.plane[0];  // Y unchanged (COW share)
    out.plane[1] = QByteArray(out.stride[1] * chromaH, Qt::Uninitialized);
    char* dst = out.plane[1].data();
    const char* u = in.plane[1].constData();
    const char* v = in.plane[2].constData();
    for (int row = 0; row < chromaH; ++row) {
        char* d = dst + row * out.stride[1];
        const char* us = u + row * in.stride[1];
        const char* vs = v + row * in.stride[2];
        for (int x = 0; x < chromaW; ++x) {
            d[2 * x] = us[x];
            d[2 * x + 1] = vs[x];
        }
    }
    return out;
}

CpuPlanes convertNv12ToYuv420p(const CpuPlanes& in) {
    CpuPlanes out;
    out.format = FramePixelFormat::Yuv420p;
    out.width = in.width;
    out.height = in.height;
    const int chromaW = (in.width + 1) / 2;
    const int chromaH = (in.height + 1) / 2;
    out.stride[0] = in.width;
    out.stride[1] = chromaW;
    out.stride[2] = chromaW;
    out.plane[0] = in.plane[0];  // Y unchanged (COW share)
    out.plane[1] = QByteArray(chromaW * chromaH, Qt::Uninitialized);
    out.plane[2] = QByteArray(chromaW * chromaH, Qt::Uninitialized);
    char* uu = out.plane[1].data();
    char* vv = out.plane[2].data();
    const char* src = in.plane[1].constData();
    for (int row = 0; row < chromaH; ++row) {
        const char* s = src + row * in.stride[1];
        for (int x = 0; x < chromaW; ++x) {
            uu[row * chromaW + x] = s[2 * x];
            vv[row * chromaW + x] = s[2 * x + 1];
        }
    }
    return out;
}

}  // namespace

CpuFrameData::CpuFrameData(CpuPlanes planes) : m_planes(std::move(planes)) {}

CpuPlanes CpuFrameData::readToCpu(FramePixelFormat target) const {
    if (target == m_planes.format) return m_planes;  // no-op COW passthrough
    if (m_planes.format == FramePixelFormat::Yuv420p && target == FramePixelFormat::Nv12)
        return convertYuv420pToNv12(m_planes);
    if (m_planes.format == FramePixelFormat::Nv12 && target == FramePixelFormat::Yuv420p)
        return convertNv12ToYuv420p(m_planes);
    return m_planes;  // Rgba8 / unsupported cross-convert: return as-is (Phase 1 never hits this)
}

FrameHandle makeCpuFrameHandle(CpuPlanes planes, FrameMetadata meta) {
    auto data = std::make_shared<CpuFrameData>(std::move(planes));
    return FrameHandle(std::move(data), std::move(meta));
}

FrameHandle solidYuv420pHandle(int width, int height, uchar y, uchar u, uchar v) {
    const int chromaW = (width + 1) / 2;
    const int chromaH = (height + 1) / 2;
    CpuPlanes p;
    p.format = FramePixelFormat::Yuv420p;
    p.width = width;
    p.height = height;
    p.stride[0] = width;
    p.stride[1] = chromaW;
    p.stride[2] = chromaW;
    p.plane[0] = QByteArray(width * height, char(y));
    p.plane[1] = QByteArray(chromaW * chromaH, char(u));
    p.plane[2] = QByteArray(chromaW * chromaH, char(v));

    FrameMetadata m;
    m.key.format = FramePixelFormat::Yuv420p;
    m.key.width = width;
    m.key.height = height;
    m.stride[0] = p.stride[0];
    m.stride[1] = p.stride[1];
    m.stride[2] = p.stride[2];
    return makeCpuFrameHandle(std::move(p), std::move(m));
}
```

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_framehandle && ctest --test-dir build/c -R tst_framehandle --output-on-failure
```
Expected: PASS (9 tests) — including `aliasedHandleMetadataOverrideKeepsPixelsByteIdentical` (the keystone invariant).

- [ ] **Step 5: Commit**

```sh
git add playback/output/framehandle.h playback/output/framehandle.cpp tests/unit/tst_framehandle.cpp
git commit -m "feat(frame-handle): IFrameData/CpuFrameData/FrameHandle + aliasing-immutability gate"
```

---

## Task 5: `MediaVideoFrameView` compat value-view

**Files:**
- Modify: `playback/output/framehandle.h`, `playback/output/framehandle.cpp`
- Test: `tests/unit/tst_framehandle.cpp` (add a slot)

**Interfaces:**
- Consumes: `FrameHandle`, `CpuPlanes` (Task 4).
- Produces:
  ```cpp
  // A thin MediaVideoFrame-compatible VALUE view backed by readToCpu(Yuv420p):
  // exposes width/height/feedIndex/ptsMs/outputFrameIndex/isPlaceholder plus
  // planeY/planeU/planeV and strideY/strideU/strideV, so the ~9-10 unit-test
  // files (and product sites) that index .planeY/.planeU/.planeV migrate
  // mechanically. Constructed from a FrameHandle; reads pixels once via
  // readToCpu(Yuv420p).
  struct MediaVideoFrameView {
      explicit MediaVideoFrameView(const FrameHandle& h);
      int feedIndex; qint64 ptsMs; qint64 outputFrameIndex;
      int width; int height; bool isPlaceholder;
      QByteArray planeY, planeU, planeV;
      int strideY, strideU, strideV;
      bool isValid() const;
  };
  ```

> The view is the permanent plane-indexing read path (not a temporary scaffold). It pulls `readToCpu(Yuv420p)` once at construction; the planes are COW shares of the handle's CPU pixels for the no-op case, so constructing a view does not deep-copy.

- [ ] **Step 1: Write the failing test**

Add to `tests/unit/tst_framehandle.cpp`:

```cpp
    void mediaVideoFrameViewMirrorsLegacyFields();
```

```cpp
void TestFrameHandle::mediaVideoFrameViewMirrorsLegacyFields() {
    FrameHandle h = solidYuv420pHandle(4, 4, 70, 90, 110);
    h.metadata().key.feedIndex = 2;
    h.metadata().key.ptsMs = 123;
    h.metadata().outputFrameIndex = 7;
    h.metadata().key.isPlaceholder = false;

    MediaVideoFrameView v(h);
    QCOMPARE(v.feedIndex, 2);
    QCOMPARE(v.ptsMs, qint64(123));
    QCOMPARE(v.outputFrameIndex, qint64(7));
    QCOMPARE(v.width, 4);
    QCOMPARE(v.height, 4);
    QVERIFY(!v.isPlaceholder);
    QCOMPARE(v.strideY, 4);
    QCOMPARE(v.strideU, 2);
    QCOMPARE(v.strideV, 2);
    QCOMPARE(uchar(v.planeY.at(0)), uchar(70));
    QCOMPARE(uchar(v.planeU.at(0)), uchar(90));
    QCOMPARE(uchar(v.planeV.at(0)), uchar(110));
    QVERIFY(v.isValid());

    // No-pixel handle yields an invalid view.
    FrameHandle empty;
    MediaVideoFrameView ev(empty);
    QVERIFY(!ev.isValid());
}
```

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_framehandle
```
Expected: FAIL to compile — `MediaVideoFrameView` undeclared.

- [ ] **Step 3: Write the implementation**

Append to `playback/output/framehandle.h` (before `#endif`):

```cpp
// A thin MediaVideoFrame-compatible value view over a FrameHandle, backed by
// readToCpu(Yuv420p). It exposes the legacy field names (planeY/U/V, strideY/U/V,
// feedIndex, ptsMs, outputFrameIndex, width, height, isPlaceholder) so the
// plane-indexing access sites migrate mechanically. This is the permanent CPU
// read path, not a migration scaffold.
struct MediaVideoFrameView {
    explicit MediaVideoFrameView(const FrameHandle& h);

    int feedIndex = -1;
    qint64 ptsMs = 0;
    qint64 outputFrameIndex = -1;
    int width = 0;
    int height = 0;
    bool isPlaceholder = false;
    QByteArray planeY;
    QByteArray planeU;
    QByteArray planeV;
    int strideY = 0;
    int strideU = 0;
    int strideV = 0;

    bool isValid() const {
        return width > 0 && height > 0 && !planeY.isEmpty() && !planeU.isEmpty() &&
               !planeV.isEmpty();
    }
};
```

Append to `playback/output/framehandle.cpp`:

```cpp
MediaVideoFrameView::MediaVideoFrameView(const FrameHandle& h) {
    const FrameMetadata& m = h.metadata();
    feedIndex = m.key.feedIndex;
    ptsMs = m.key.ptsMs;
    outputFrameIndex = m.outputFrameIndex;
    width = m.key.width;
    height = m.key.height;
    isPlaceholder = m.key.isPlaceholder;
    if (h.isNull()) return;
    const CpuPlanes p = h.readToCpu(FramePixelFormat::Yuv420p);
    planeY = p.plane[0];
    planeU = p.plane[1];
    planeV = p.plane[2];
    strideY = p.stride[0];
    strideU = p.stride[1];
    strideV = p.stride[2];
}
```

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_framehandle && ctest --test-dir build/c -R tst_framehandle --output-on-failure
```
Expected: PASS (10 tests).

- [ ] **Step 5: Commit**

```sh
git add playback/output/framehandle.h playback/output/framehandle.cpp tests/unit/tst_framehandle.cpp
git commit -m "feat(frame-handle): MediaVideoFrameView compat value-view over readToCpu"
```

---

## Task 6: Migrate `OutputFrameCache` to store/return `FrameHandle` refs (D9)

**Files:**
- Modify: `playback/output/outputframecache.h`, `playback/output/outputframecache.cpp`
- Test: `tests/unit/tst_outputframecache.cpp` (mechanical migration via the view)

**Interfaces:**
- Consumes: `FrameHandle` (Task 4).
- Produces (new API — downstream depends on these exact signatures):
  ```cpp
  void insertVideoFrame(const FrameHandle& frame);
  std::optional<FrameHandle> videoFrameAt(int feedIndex, qint64 playheadMs) const;
  FrameHandle videoFrameOrPlaceholder(int feedIndex, qint64 playheadMs) const;
  ```
  Audio API (`insertAudioFrame`, `audioSpanOrSilence`) is unchanged. Storage holds `FrameHandle` (refcount bumps), never `CpuPlanes` copies (D9).

- [ ] **Step 1: Migrate the test source**

In `tests/unit/tst_outputframecache.cpp`, every site that builds a `MediaVideoFrame` to insert becomes a `FrameHandle` via `solidYuv420pHandle` + metadata, and every site that reads `.planeY/.ptsMs/...` wraps the returned handle in `MediaVideoFrameView`. Concretely, replace the local frame factory and every read. For example a helper:

```cpp
#include "playback/output/framehandle.h"

static FrameHandle frame(int feed, qint64 pts, uchar y) {
    FrameHandle h = solidYuv420pHandle(4, 4, y, 128, 128);
    h.metadata().key.feedIndex = feed;
    h.metadata().key.ptsMs = pts;
    return h;
}
```

and at every read site:

```cpp
auto opt = cache.videoFrameAt(0, 100);
QVERIFY(opt.has_value());
MediaVideoFrameView v(*opt);
QCOMPARE(uchar(v.planeY.at(0)), uchar(10));
QCOMPARE(v.ptsMs, qint64(100));
```

Migrate **every** assertion in the file this way; assertion *values* stay identical.

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_outputframecache
```
Expected: FAIL to compile — `insertVideoFrame` / `videoFrameAt` still take/return `MediaVideoFrame`, not `FrameHandle`.

- [ ] **Step 3: Migrate the implementation**

In `playback/output/outputframecache.h`, replace the include and signatures:

```cpp
#include "playback/output/framehandle.h"
#include "playback/output/mediaframe.h"  // MediaAudioFrame still lives here
```

```cpp
    void insertVideoFrame(const FrameHandle& frame);
    std::optional<FrameHandle> videoFrameAt(int feedIndex, qint64 playheadMs) const;
    FrameHandle videoFrameOrPlaceholder(int feedIndex, qint64 playheadMs) const;
```

and the storage:

```cpp
    QVector<QVector<FrameHandle>> m_video;
```

In `playback/output/outputframecache.cpp`, update `insertVideoFrame` to key sort/uniqueness on `frame.metadata().key.feedIndex` and `frame.metadata().key.ptsMs` (replacing the prior `frame.feedIndex` / `frame.ptsMs`), store the `FrameHandle` by value (a refcount bump), and update `videoFrameAt` to return the stored handle. `videoFrameOrPlaceholder` builds a placeholder via `solidYuv420pHandle(m_placeholderWidth, m_placeholderHeight, 16, 128, 128)` with `metadata().key.isPlaceholder = true` and `metadata().key.feedIndex = feedIndex`. `mergeFrom` and `trimBefore` operate on the handle vectors unchanged in logic (the trim key is `metadata().key.ptsMs`). Preserve the exact insert/sort/replace-by-pts semantics; only the element type changes.

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_outputframecache && ctest --test-dir build/c -R tst_outputframecache --output-on-failure
```
Expected: PASS with identical assertion values.

- [ ] **Step 5: Commit**

```sh
git add playback/output/outputframecache.h playback/output/outputframecache.cpp tests/unit/tst_outputframecache.cpp
git commit -m "refactor(frame-handle): OutputFrameCache stores/returns FrameHandle refs (D9)"
```

---

## Task 7: Migrate `TrackBuffer` to hold `FrameHandle` refs

**Files:**
- Modify: `playback/trackbuffer.h`, `playback/trackbuffer.cpp`
- Test: `tests/unit/tst_trackbuffer.cpp` (mechanical migration via the view)

**Interfaces:**
- Consumes: `FrameHandle` (Task 4).
- Produces:
  ```cpp
  struct Frame { int64_t ptsMs = -1; FrameHandle frame; };
  bool insert(int64_t ptsMs, const FrameHandle& f, int capFrames, int64_t keepNearMs, int64_t protectToMs);
  bool frameAt(int64_t playheadMs, FrameHandle& out, int64_t& outPtsMs) const;
  ```

- [ ] **Step 1: Migrate the test source**

In `tests/unit/tst_trackbuffer.cpp`, build inserted frames as `FrameHandle` (via `solidYuv420pHandle` + metadata) and read results through `MediaVideoFrameView`. The cap/eviction/trim assertions are on `ptsMs` and `size()` — those are unchanged; only the element construction and any `.planeY`/`.width` read sites move to the view.

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_trackbuffer
```
Expected: FAIL to compile — `insert` / `frameAt` / `Frame.frame` still use `MediaVideoFrame`.

- [ ] **Step 3: Migrate the implementation**

In `playback/trackbuffer.h`, swap `#include "playback/output/mediaframe.h"` → `#include "playback/output/framehandle.h"`, change `Frame.frame` to `FrameHandle`, and change `insert`/`frameAt` signatures to take/return `FrameHandle`. In `playback/trackbuffer.cpp`, the body is unchanged except the stored/copied element type is now `FrameHandle` (a refcount bump on insert/replace). All ordering/cap/trim logic keys on `ptsMs`, which is unchanged.

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_trackbuffer && ctest --test-dir build/c -R tst_trackbuffer --output-on-failure
```
Expected: PASS with identical assertion values.

- [ ] **Step 5: Commit**

```sh
git add playback/trackbuffer.h playback/trackbuffer.cpp tests/unit/tst_trackbuffer.cpp
git commit -m "refactor(frame-handle): TrackBuffer::Frame holds a FrameHandle ref"
```

---

## Task 8: Migrate `Yuv420pCompositor` to handles

**Files:**
- Modify: `playback/output/yuv420pcompositor.h`, `playback/output/yuv420pcompositor.cpp`
- Test: `tests/unit/tst_yuv420pcompositor.cpp`

**Interfaces:**
- Consumes: `FrameHandle`, `MediaVideoFrameView`, `solidYuv420pHandle` (Task 4/5).
- Produces:
  ```cpp
  static FrameHandle composeGrid(const QList<FrameHandle>& frames, int width, int height);
  ```

> The composite stays a CPU nearest-neighbor blit (the permanent oracle path). It reads each input through `MediaVideoFrameView` (the plane-indexing read path), composites into a fresh `CpuPlanes`, and wraps the result in a handle via `makeCpuFrameHandle`. Output pixels are byte-identical to today.

- [ ] **Step 1: Migrate the test source**

In `tests/unit/tst_yuv420pcompositor.cpp`, build the input `QList<FrameHandle>` with `solidYuv420pHandle(...)` (+ width/height set to the per-source sizes the test uses), call `composeGrid`, and read the result via `MediaVideoFrameView` for every `.planeY/.planeU/.planeV` assertion. Values stay identical.

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_yuv420pcompositor
```
Expected: FAIL to compile — `composeGrid` still takes `QList<MediaVideoFrame>`.

- [ ] **Step 3: Migrate the implementation**

In `playback/output/yuv420pcompositor.h`, change the signature to `static FrameHandle composeGrid(const QList<FrameHandle>& frames, int width, int height);` and include `framehandle.h`. In `playback/output/yuv420pcompositor.cpp`:

```cpp
FrameHandle Yuv420pCompositor::composeGrid(const QList<FrameHandle>& frames, int width,
                                           int height) {
    FrameHandle outHandle = solidYuv420pHandle(width, height, 16, 128, 128);
    outHandle.metadata().key.feedIndex = -1;
    CpuPlanes out = outHandle.readToCpu(FramePixelFormat::Yuv420p);

    const int count = qMax(1, frames.size());
    const int columns = qMax(1, int(std::ceil(std::sqrt(double(count)))));
    const int rows = qMax(1, int(std::ceil(double(count) / double(columns))));

    for (int i = 0; i < frames.size(); ++i) {
        const MediaVideoFrameView frame(frames.at(i));
        if (!frame.isValid()) continue;
        const int col = i % columns;
        const int row = i / columns;
        const int dstX = col * width / columns;
        const int dstY = row * height / rows;
        const int dstRight = (col + 1) * width / columns;
        const int dstBottom = (row + 1) * height / rows;
        const int dstW = qMax(0, dstRight - dstX);
        const int dstH = qMax(0, dstBottom - dstY);

        scalePlaneNearest(frame.planeY, frame.strideY, frame.width, frame.height, out.plane[0],
                          out.stride[0], dstX, dstY, dstW, dstH);

        const int srcChromaW = (frame.width + 1) / 2;
        const int srcChromaH = (frame.height + 1) / 2;
        const int dstChromaX = dstX / 2;
        const int dstChromaY = dstY / 2;
        const int dstChromaRight = (dstRight + 1) / 2;
        const int dstChromaBottom = (dstBottom + 1) / 2;
        const int dstChromaW = qMax(0, dstChromaRight - dstChromaX);
        const int dstChromaH = qMax(0, dstChromaBottom - dstChromaY);
        scalePlaneNearest(frame.planeU, frame.strideU, srcChromaW, srcChromaH, out.plane[1],
                          out.stride[1], dstChromaX, dstChromaY, dstChromaW, dstChromaH);
        scalePlaneNearest(frame.planeV, frame.strideV, srcChromaW, srcChromaH, out.plane[2],
                          out.stride[2], dstChromaX, dstChromaY, dstChromaW, dstChromaH);
    }

    FrameMetadata meta = outHandle.metadata();
    return makeCpuFrameHandle(std::move(out), std::move(meta));
}
```

`scalePlaneNearest` is unchanged (it already takes `QByteArray&` for dst).

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_yuv420pcompositor && ctest --test-dir build/c -R tst_yuv420pcompositor --output-on-failure
```
Expected: PASS with identical pixel goldens.

- [ ] **Step 5: Commit**

```sh
git add playback/output/yuv420pcompositor.h playback/output/yuv420pcompositor.cpp tests/unit/tst_yuv420pcompositor.cpp
git commit -m "refactor(frame-handle): Yuv420pCompositor composes FrameHandle -> FrameHandle"
```

---

## Task 9: Migrate `OutputBusEngine` (the metadata-mutation seam)

**Files:**
- Modify: `playback/output/outputbusengine.h`, `playback/output/outputbusengine.cpp`
- Test: `tests/unit/tst_outputbusengine.cpp` (mechanical migration via the view)

**Interfaces:**
- Consumes: `FrameHandle`, `MediaVideoFrameView`, the migrated cache/compositor (Tasks 6/8).
- Produces:
  ```cpp
  struct OutputBusFrame { ...; FrameHandle video; MediaAudioFrame audio; ...; };
  struct MultiviewComposite { bool valid; QVector<qint64> sourceKeys; FrameHandle video; };
  ```
  `videoHashFor` now takes the `FramePayloadKey` (or the handle's metadata key); `outputFrameIdentityFor` reads `frame.video.metadata().key`.

> **The load-bearing change:** today `renderMultiview` and `renderSingleSource` **mutate** `out.video.ptsMs` / `out.video.outputFrameIndex` after composing/copying a COW-shared `MediaVideoFrame` (outputbusengine.cpp:154-155, :181). With an immutable handle, those writes move to `out.video.metadata()` — the cheap per-handle override — leaving shared pixels untouched. This is exactly the aliasing contract proven in Task 4.

- [ ] **Step 1: Migrate the test source**

In `tests/unit/tst_outputbusengine.cpp`, the `video(...)`/`videoYuv(...)` helpers return `FrameHandle` (via `solidYuv420pHandle` + `metadata().key.feedIndex/ptsMs`), and every assertion that reads `.video.planeY.at(0)` etc. wraps the result handle in a `MediaVideoFrameView`. For example:

```cpp
static FrameHandle videoYuv(int feed, qint64 pts, uchar y, uchar u, uchar v) {
    FrameHandle h = solidYuv420pHandle(4, 4, y, u, v);
    h.metadata().key.feedIndex = feed;
    h.metadata().key.ptsMs = pts;
    return h;
}
```

```cpp
auto feed0 = engine.renderFeed(0, 3, state, cache);
MediaVideoFrameView v0(feed0.video);
QCOMPARE(uchar(v0.planeY.at(0)), uchar(10));
```

The identity assertions (`feed0.identity.videoHash`, `samePayloadAs`, multiview identity) read `feed0.identity` as before — `OutputFrameIdentity` is unchanged.

- [ ] **Step 2: Run the test, expect FAIL**

```sh
cmake --build build/c --target tst_outputbusengine
```
Expected: FAIL to compile — `OutputBusFrame.video` / `MultiviewComposite.video` and the `cache.videoFrameAt` return type changed; `out.video.ptsMs` writes no longer compile.

- [ ] **Step 3: Migrate the implementation**

In `playback/output/outputbusengine.h`: include `framehandle.h`; change `OutputBusFrame.video` and `MultiviewComposite.video` to `FrameHandle`. In `playback/output/outputbusengine.cpp`:

- `videoHashFor` takes the metadata key and hashes the same fields it does today (feedIndex, ptsMs, width, height, format, isPlaceholder) — reading them from `key.feedIndex`, `key.ptsMs`, etc. Note `format` is now `FramePixelFormat`; cast `int(key.format)` exactly as the prior `int(video.format)` so the hash value is *byte-stable only if the enum ordinal is stable*. **Guard the golden:** `FramePixelFormat::Yuv420p` is ordinal 1, matching the prior `MediaPixelFormat::Yuv420p` ordinal 1 — assert this in a Task-9 unit check so the hash is unchanged (see Step 1 add-on below).
- `outputFrameIdentityFor` reads `frame.video.metadata().key.feedIndex`, `.ptsMs`, `.isPlaceholder`.
- `renderSingleSource`: `out.video = cache.videoFrameOrPlaceholder(...)` (a handle); the placeholder branch builds `solidYuv420pHandle(...)` with `metadata().key.isPlaceholder = true`. Replace `out.video.outputFrameIndex = outputFrameIndex;` with `out.video.metadata().outputFrameIndex = outputFrameIndex;` and `out.video.ptsMs = ...` with `out.video.metadata().key.ptsMs = ...`.
- `renderMultiview`: `cache.videoFrameAt(feed, ...)` returns `std::optional<FrameHandle>`; read `src->metadata().key.ptsMs`. `composeGrid` takes `QList<FrameHandle>`. The memo reuse `out.video = memo->video;` is now a handle refcount bump (the D9 ref-share). The post-compose mutations become `out.video.metadata().key.ptsMs = sourcePtsMs;` and `out.video.metadata().outputFrameIndex = outputFrameIndex;`.

**Step-1 add-on (enum-ordinal golden guard):** add to `tests/unit/tst_framehandle.cpp`:

```cpp
    void yuv420pOrdinalMatchesLegacyForHashStability();
```
```cpp
#include "playback/output/outputtypes.h"
void TestFrameHandle::yuv420pOrdinalMatchesLegacyForHashStability() {
    // videoHashFor hashes int(format); the migrated identity hash stays
    // byte-stable only if Yuv420p keeps ordinal 1 (== MediaPixelFormat::Yuv420p).
    QCOMPARE(int(FramePixelFormat::Yuv420p), int(MediaPixelFormat::Yuv420p));
}
```

- [ ] **Step 4: Run the test, expect PASS**

```sh
cmake --build build/c --target tst_outputbusengine tst_framehandle && \
  ctest --test-dir build/c -R "tst_outputbusengine|tst_framehandle" --output-on-failure
```
Expected: PASS — identity hashes, PGM pixel-exact copy, multiview memo all match prior values.

- [ ] **Step 5: Commit**

```sh
git add playback/output/outputbusengine.h playback/output/outputbusengine.cpp tests/unit/tst_outputbusengine.cpp tests/unit/tst_framehandle.cpp
git commit -m "refactor(frame-handle): OutputBusEngine renders FrameHandle; metadata override replaces pixel mutation"
```

---

## Task 10: Migrate the sinks (NDI + Qt preview) to read via the view

**Files:**
- Modify: `playback/output/ndisink.cpp`, `playback/output/qtpreviewsink.cpp`, `playback/output/qtpreviewsink.h`
- Test: `tests/unit/tst_ndisink.cpp`, `tests/unit/tst_qtpreviewsink.cpp`, `tests/unit/tst_outputdispatcher.cpp`, `tests/unit/tst_outputdispatcher_holdlast.cpp`, `tests/unit/tst_outputruntime.cpp`, `tests/unit/tst_sharedcacheslot.cpp`

**Interfaces:**
- Consumes: `OutputBusFrame.video` (now a `FrameHandle`), `MediaVideoFrameView`.
- The sink public method signatures (`submit(const OutputBusFrame&)`) are **unchanged**; only the internal pixel read moves to the view. NDI `packI420` and Qt `toQVideoFrame` take a `MediaVideoFrameView` (or a `FrameHandle`) instead of a `MediaVideoFrame`.

- [ ] **Step 1: Migrate the test sources**

In each listed test, replace direct `MediaVideoFrame` construction of `OutputBusFrame.video` with `solidYuv420pHandle(...)` (+ metadata), and read any `.planeY/.width/...` through `MediaVideoFrameView`. `tst_outputdispatcher*` and `tst_outputruntime` build `OutputBusFrame`s; set `.video = solidYuv420pHandle(...)` with the metadata they need for identity. `tst_sharedcacheslot` builds an `OutputFrameCache` and inserts handles (already migrated in Task 6) — update its frame factory.

- [ ] **Step 2: Run the tests, expect FAIL**

```sh
cmake --build build/c --target tst_ndisink tst_qtpreviewsink tst_outputdispatcher tst_outputdispatcher_holdlast tst_outputruntime tst_sharedcacheslot
```
Expected: FAIL to compile — `frame.video` is a `FrameHandle`; `.planeY` etc. not members; `packI420`/`toQVideoFrame` take `MediaVideoFrame`.

- [ ] **Step 3: Migrate the implementations**

- `playback/output/ndisink.cpp`: change `packI420` to take `const MediaVideoFrameView& frame` (include `framehandle.h`); its body is unchanged (it already reads `frame.planeY/strideY/width/height` and uses the `isValid()` accessor — the view provides all of these). At the call site in `submit`, construct `MediaVideoFrameView view(busFrame.video);` and pass it.
- `playback/output/qtpreviewsink.cpp` / `.h`: change `QtPreviewSink::deliver` and `toQVideoFrame` to take a `const FrameHandle&` (or build a `MediaVideoFrameView` inside). `deliver` constructs `MediaVideoFrameView frame(handle);`, and `toQVideoFrame` reads `frame.planeY/...` exactly as today. The `height > 576` color-space pick is unchanged (Phase 1 keeps the heuristic — `color-metadata` replaces it later). `QtPreviewOutputSink::submit` passes `frame.video` (a `FrameHandle`).

- [ ] **Step 4: Run the tests, expect PASS**

```sh
cmake --build build/c --target tst_ndisink tst_qtpreviewsink tst_outputdispatcher tst_outputdispatcher_holdlast tst_outputruntime tst_sharedcacheslot && \
  ctest --test-dir build/c -R "tst_ndisink|tst_qtpreviewsink|tst_outputdispatcher|tst_outputruntime|tst_sharedcacheslot" --output-on-failure
```
Expected: PASS with identical assertion values.

- [ ] **Step 5: Commit**

```sh
git add playback/output/ndisink.cpp playback/output/qtpreviewsink.cpp playback/output/qtpreviewsink.h tests/unit/tst_ndisink.cpp tests/unit/tst_qtpreviewsink.cpp tests/unit/tst_outputdispatcher.cpp tests/unit/tst_outputdispatcher_holdlast.cpp tests/unit/tst_outputruntime.cpp tests/unit/tst_sharedcacheslot.cpp
git commit -m "refactor(frame-handle): NDI + Qt preview sinks read pixels via MediaVideoFrameView"
```

---

## Task 11: Migrate `PlaybackWorker` (decode edge + D9 ref-sharing)

**Files:**
- Modify: `playback/playbackworker.cpp`, `playback/playbackworker.h`
- Test: covered by `e2e_play` (the worker has no isolated unit; the e2e suite is the gate).

**Interfaces:**
- Consumes: `FrameHandle`, the migrated `TrackBuffer` / `OutputFrameCache` (Tasks 6/7).
- Produces:
  ```cpp
  FrameHandle PlaybackWorker::convertToMediaVideoFrame(AVFrame* frame, int feedIndex);
  ```
  (rename optional; keep the name to bound the diff — it now returns a handle). **D9:** the same `FrameHandle` produced once at decode is inserted into **both** `track->buffer` and `m_outputCache` as a refcount bump (today they were two `MediaVideoFrame` copies, playbackworker.cpp:621-627); the inactive-graph snapshot (:2247-2256) inserts the buffer's existing handle refs, never re-encoding pixels.

- [ ] **Step 1: Migrate `convertToMediaVideoFrame` to return a handle**

In `playback/playbackworker.h`, change the declaration return type to `FrameHandle` (include `playback/output/framehandle.h`). In `playback/playbackworker.cpp`, the body builds a `CpuPlanes` (the existing per-plane `memcpy` into `QByteArray`s, unchanged) and a `FrameMetadata` (feedIndex, width, height, `format = FramePixelFormat::Yuv420p`, strides), then returns `makeCpuFrameHandle(std::move(planes), std::move(meta))`. The `AV_PIX_FMT_YUV420P` guard returning an invalid result becomes `return FrameHandle();` (a null handle the caller skips).

- [ ] **Step 2: Update the two decode insert sites (D9 ref-share)**

At playbackworker.cpp:621-627 and :694-702, the decoded `FrameHandle mediaFrame` is inserted into `track->buffer.insert(framePtsMs, mediaFrame, ...)` and `m_outputCache->insertVideoFrame(mediaFrame)` — both now refcount bumps of the *same* shared pixels (no second copy). Set `mediaFrame.metadata().key.feedIndex = track->feedIndex` before insert (the cache keys on it). The preroll-staging sites (:1324-1328, :1352-1355) insert the same way into `m_prerollStagingCache`.

- [ ] **Step 3: Update the inactive-graph snapshot (D9 refs, not copies)**

At playbackworker.cpp:2236-2256, the snapshot iterates `track->buffer.framesSnapshot()` (now `QVector<TrackBuffer::Frame>` with `FrameHandle frame`). For placeholder-geometry detection, read `MediaVideoFrameView v(frame.frame); if (v.isValid()) { placeholderWidth = v.width; ... }`. The cache fill `cacheHolder->insertVideoFrame(frame.frame)` is now a refcount bump of the buffer's handle (set `frame.frame.metadata().key.feedIndex = track->feedIndex` first). The other `MediaVideoFrame f; ref->buffer.frameAt(P, f, p)` sites (:1847-1850, :2273-2275) declare `FrameHandle f;` and read via the view where pixels are needed.

- [ ] **Step 4: Build the app + run the e2e gate**

```sh
cmake --build build/c
ctest --test-dir build/c -R e2e_play --output-on-failure
```
Expected: PASS — `play1x seekplay reverse stepscrub sliderscrub liveedge seekflash farback armedcut*` all green with identical counter thresholds (`placeholderFramesDelta`, `heldFramesDelta`, `maxClockDivergenceMs`, `decodedVideoFrames`, `stagingVideoFramesDecoded`). Run a couple of scenarios directly on distinct SRT ports if needed:

```sh
tests/e2e/run_playback_e2e.sh <play_harness> <record_harness> play1x 1 9001
tests/e2e/run_playback_e2e.sh <play_harness> <record_harness> armedcut 4 9002
```

- [ ] **Step 5: Commit**

```sh
git add playback/playbackworker.cpp playback/playbackworker.h
git commit -m "refactor(frame-handle): PlaybackWorker decode edge yields FrameHandle; buffer+cache share refs (D9)"
```

---

## Task 12: Retire `MediaVideoFrame`; full-suite zero-regression gate

**Files:**
- Modify: `playback/output/mediaframe.h` (drop `MediaVideoFrame`; keep `MediaAudioFrame` + `silentS16Stereo`)
- Modify: any remaining include sites flagged by the build.

**Interfaces:**
- After this task `MediaVideoFrame` no longer exists; the compat read path is `MediaVideoFrameView` and the value type is `FrameHandle`. `MediaAudioFrame` and `silentS16Stereo` stay in `mediaframe.h` (audio is out of scope for this subproject).

- [ ] **Step 1: Remove the `MediaVideoFrame` struct**

In `playback/output/mediaframe.h`, delete the `MediaVideoFrame` struct (lines 9-43) and the now-unused `#include "playback/output/outputtypes.h"` only if nothing else in the file needs it (it does not after the struct is gone — but `MediaSampleFormat` for `MediaAudioFrame` lives in `outputtypes.h`, so keep the include). Keep `MediaAudioFrame`, `silentS16Stereo`.

- [ ] **Step 2: Build everything — let the compiler find stragglers**

```sh
cmake --build build/c 2>&1 | tee /tmp/frame-handle-build.log
```
Expected: any remaining `MediaVideoFrame` reference is a compile error pointing at a missed site. For each, migrate it to `FrameHandle` + `MediaVideoFrameView` per the patterns in Tasks 6-11. Re-run until clean. (If the build is already clean, no straggler remains.)

- [ ] **Step 3: Run the FULL unit + e2e suite (the gate)**

```sh
QT_QPA_PLATFORM=offscreen ctest --test-dir build/c -L unit --output-on-failure
ctest --test-dir build/c -R e2e_play --output-on-failure
```
Expected: **every** unit test and every `e2e_play` scenario passes with identical assertion values and golden outputs. This is the zero-regression gate the whole subproject exists to satisfy.

- [ ] **Step 4: Format changed lines + sanitizer spot-check**

```sh
CF=/opt/homebrew/opt/llvm/bin/clang-format
GCF=/opt/homebrew/opt/llvm/bin/git-clang-format
python3 "$GCF" --binary "$CF" --diff --commit origin/main -- '*.cpp' '*.h'
```
Apply formatting to changed lines only if the diff is non-empty (stage first, then run without `--diff`). Build once under ASan/UBSan if a sanitizer preset is available and run `tst_framehandle` + a couple of e2e scenarios to confirm no new findings (the aliasing/refcount paths are the risk surface).

- [ ] **Step 5: Commit + request independent review**

```sh
git add playback/output/mediaframe.h
git commit -m "refactor(frame-handle): retire MediaVideoFrame value type; FrameHandle is the universal frame"
```

Per CLAUDE.md, the playback-worker threading touched in Task 11 (D9 ref-sharing across `m_bufferMutex`-protected structures) requires an independent review before merge. Open the PR and request it.

---

## Migration site inventory (reference)

Product sites carrying `MediaVideoFrame` today (all migrated by Tasks 6-12):

- `playback/output/mediaframe.h` — the struct itself (Task 12 removes it).
- `playback/output/outputframecache.{h,cpp}` — storage + 3 accessors (Task 6).
- `playback/trackbuffer.{h,cpp}` — `Frame.frame` + `insert`/`frameAt` (Task 7).
- `playback/output/yuv420pcompositor.{h,cpp}` — `composeGrid` (Task 8).
- `playback/output/outputbusengine.{h,cpp}` — `OutputBusFrame.video`, `MultiviewComposite.video`, `videoHashFor`, `outputFrameIdentityFor`, the 2 metadata-mutation seams (Task 9).
- `playback/output/ndisink.cpp` — `packI420` (Task 10).
- `playback/output/qtpreviewsink.{h,cpp}` — `deliver`/`toQVideoFrame` (Task 10).
- `playback/playbackworker.{cpp,h}` — `convertToMediaVideoFrame`, the 2 live decode inserts, the 2 preroll inserts, the inactive-graph snapshot, the `frameAt` read sites (Task 11).

Test sites indexing `.planeY/.planeU/.planeV` (migrated via `MediaVideoFrameView`): `tst_outputbusengine.cpp`, `tst_outputframecache.cpp`, `tst_trackbuffer.cpp`, `tst_yuv420pcompositor.cpp`, `tst_ndisink.cpp`, `tst_outputdispatcher.cpp`, `tst_outputdispatcher_holdlast.cpp`, `tst_outputruntime.cpp`, `tst_sharedcacheslot.cpp` (9 files).
</content>
</invoke>
