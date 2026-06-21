# Color Metadata Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (- [ ]) syntax.

**Goal:** Plumb a real `ColorMetadata` (matrix/range/primaries/transfer/chroma/bitDepth) from the decode edge through `FrameMetadata` to the Qt preview sink, replacing the height>576 color-space guess in [qtpreviewsink.cpp:23](../../../playback/output/qtpreviewsink.cpp) — as a **proven no-op on existing goldens** in Phase 1.

**Architecture:** A pure, fully testable SPS-VUI color parser (`colorvui.{h,cpp}`) and a default-tagging policy (`colormetadatapolicy.{h,cpp}`) feed a `ColorMetadata` into every decoded frame's `FrameMetadata.color` at `PlaybackWorker::convertToMediaVideoFrame`. The Apple path additionally reads `CMFormatDescription` color tags (platform-gated `.mm`). `QtPreviewSink::toQVideoFrame` then maps `metadata().color` onto the `QVideoFrameFormat` instead of inferring from height. The default-tagging policy is calibrated so untagged fixtures reproduce today's `height>576 → BT709/BT601 + Video range`, so all existing assertion values and golden outputs are byte-identical.

**Tech Stack:** C++17, Qt 6 (Core/Test/Multimedia), FFmpeg (libav* — `AVFrame` color fields), Apple CoreMedia/CoreVideo (`CMFormatDescription`), CMake + Ninja. Headless Qt Test under `QT_QPA_PLATFORM=offscreen`.

## Global Constraints

Copied verbatim from the program spec (§1, §7, §9) — these are project-wide and override local convenience:

- **Keystone-first.** This subproject depends on `frame-handle` (Phase 1 keystone). It consumes the keystone's exact public types — `FramePixelFormat`, `ColorMetadata`, `FramePayloadKey`, `FrameMetadata`, `FrameHandle`, `MediaVideoFrameView`, `makeCpuFrameHandle` — from `playback/output/colormetadata.h` and `playback/output/framehandle.h`. Do **not** invent variants of these names/signatures. `ColorMetadata` (the struct + its enums) is **defined by the keystone**; this subproject **populates and consumes** it, it does not redefine it.
- **The CPU path stays default and is the permanent correctness reference + fallback.** Everything in this subproject runs on the CPU-backed `FrameHandle` only. No GPU code.
- **Everything behind flags / behavior-preserving.** Phase 1 `color-metadata` is a **NO-OP**: default-tagging reproduces today's `height>576` heuristic so existing unit + `e2e_play` goldens are **UNCHANGED** (identical assertion values and golden outputs). No flag is introduced here because the no-op default *is* the gate.
- **No throwaways.** Every artifact (parser, policy, Apple extractor, plumbing) is production code that stays in the tree and is consumed by `gpu-compositor`/`gpu-encode` later.
- **Public-repo professionalism.** No secrets, internal notes, or references to private history. Comments document the present design, not past incidents.
- **Format changed lines only.** Several engine files use hand-written Allman style; do not reformat whole files. Run `git clang-format --binary /opt/homebrew/opt/llvm/bin/clang-format --commit origin/main -- '*.cpp' '*.h'` on changed lines before each commit.
- **The Phase-3 deliberate golden re-bake is OUT of this plan (spec §9).** When the GPU compositor later honors real tags, tagged fixtures are **re-goldened on purpose** (correct BT.601/709 + range instead of the height guess). That intentional appearance change is a *separate, later* event tracked separately and is **not** performed, scheduled, or enabled by any task here. This plan ends at a provable no-op.

### Build & test (run from the worktree root `/Users/timo.korkalainen/Development/timo/OpenLiveReplay/.claude/worktrees/gpu-phase0-2-plans`)

Configure once (fresh dir per configuration):

```sh
cmake -S . -B build/c -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos \
  -DOLR_BUILD_TESTS=ON
```

Build a target: `cmake --build build/c --target <target>`. Run a single test: `ctest --test-dir build/c -R <name> --output-on-failure`. Run the full unit label (a sink/convert change can affect siblings): `ctest --test-dir build/c -L unit --output-on-failure`. Playback e2e: `ctest --test-dir build/c -R e2e_play --output-on-failure`. Unit tests register via `olr_add_unit_test(<name> <lib>)` in [tests/unit/CMakeLists.txt](../../../tests/unit/CMakeLists.txt), where `<lib>` is one of `olr_test_core` / `olr_test_engine` / `olr_test_playback`.

### Consumed contract (from the `frame-handle` keystone — do not redefine)

This plan assumes the keystone has already landed these exact declarations:

```cpp
// playback/output/colormetadata.h
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
    bool operator==(const ColorMetadata&) const;
    bool operator!=(const ColorMetadata&) const;
};
```

```cpp
// playback/output/framehandle.h (relevant subset)
struct FrameMetadata { FramePayloadKey key; qint64 outputFrameIndex = -1; qint64 sampledPlayheadMs = 0; int stride[3] = {0,0,0}; ColorMetadata color; };
class FrameHandle {
    /* ... */
    const FrameMetadata& metadata() const;
    FrameMetadata& metadata();            // cheap per-handle override; never touches shared pixels
};
struct MediaVideoFrameView {              // MediaVideoFrame-compatible read path backed by readToCpu(Yuv420p)
    explicit MediaVideoFrameView(const FrameHandle&);
    int feedIndex; qint64 ptsMs; qint64 outputFrameIndex;
    int width; int height; bool isPlaceholder;
    QByteArray planeY, planeU, planeV; int strideY, strideU, strideV;
    bool isValid() const;
};
FrameHandle makeCpuFrameHandle(CpuPlanes planes, FrameMetadata meta);
```

> **Keystone migration already applied (consumed, not done here):** `frame-handle` migrated `PlaybackWorker::convertToMediaVideoFrame(AVFrame*, int)` to return a `FrameHandle`, and `QtPreviewSink::deliver`/`toQVideoFrame` to take a `const FrameHandle&` (read via `MediaVideoFrameView`). This plan edits those already-migrated signatures; it does **not** introduce the handle migration. If a task's "before" snapshot still shows `MediaVideoFrame`, the keystone has not landed — stop and resolve the dependency first.

---

## Task 1: SPS-VUI color parser (pure, fully testable on every platform)

A standalone bit-reader that pulls the four colour fields out of an H.264/HEVC SPS VUI: `video_full_range_flag`, `colour_primaries`, `transfer_characteristics`, `matrix_coefficients`. Mapped to keystone `ColorMetadata` enums. No FFmpeg, no platform headers — links into `olr_test_core` and runs on Linux CI too.

**Files:**
- Create: `recorder_engine/ingest/colorvui.h`, `recorder_engine/ingest/colorvui.cpp`
- Test: `tests/unit/tst_colorvui.cpp`
- Modify: `CMakeLists.txt` (add `colorvui.{h,cpp}` to the engine/core source list), `tests/unit/CMakeLists.txt` (register `tst_colorvui`)

**Interfaces:**
- Consumes: `ColorMatrix`, `ColorRange`, `ColorPrimaries`, `ColorTransfer` from `playback/output/colormetadata.h` (keystone); `NativeVideoCodec` from [recorder_engine/ingest/pespacket.h:7](../../../recorder_engine/ingest/pespacket.h).
- Produces:
  ```cpp
  // recorder_engine/ingest/colorvui.h
  struct VuiColorInfo {
      bool present = false;                          // a colour_description was found in the VUI
      ColorRange range = ColorRange::Video;          // from video_full_range_flag (0 -> Video, 1 -> Full)
      ColorPrimaries primaries = ColorPrimaries::Unspecified;
      ColorTransfer transfer = ColorTransfer::Unspecified;
      ColorMatrix matrix = ColorMatrix::Bt709;       // from matrix_coefficients; H.264 default unspecified -> Bt709
  };
  // Parse the VUI colour fields out of a raw SPS NAL payload (no start code, no
  // length prefix; the leading byte is the NAL header). Returns {present=false}
  // when the codec is unsupported, the NAL is too short, or the VUI carries no
  // colour_description_present_flag. Never throws; never reads past `nal.size()`.
  VuiColorInfo parseSpsColorVui(NativeVideoCodec codec, const QByteArray& nal);
  ```
- Mapping (ITU-T H.273 / H.264 Table E-3, H.265 Table E-3): `colour_primaries` 1->Bt709, 5/6->Bt601, 9->Bt2020, else Unspecified; `transfer_characteristics` 1/6/14/15->Bt709, 5/8->Bt601, 16->Bt2020, else Unspecified; `matrix_coefficients` 1->Bt709, 5/6->Bt601, 9/10->Bt2020, 0(RGB)/else->Bt709 (we never carry RGB matrix in 4:2:0 video; default to Bt709 for an unspecified/unknown code, matching FFmpeg's `AVCOL_SPC_UNSPECIFIED` treatment for HD).

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_colorvui.cpp`:

```cpp
// Unit tests for parseSpsColorVui — pulling colour_primaries /
// transfer_characteristics / matrix_coefficients / video_full_range_flag out
// of an H.264 SPS VUI. The fixtures are hand-built minimal SPSs whose VUI
// carries an explicit colour_description, plus the no-VUI fallback.
#include <QtTest>

#include "recorder_engine/ingest/colorvui.h"

class TestColorVui : public QObject {
    Q_OBJECT
private slots:
    void noVuiYieldsAbsent();
    void bt709FullRangeIsParsed();
    void bt601VideoRangeIsParsed();
    void tooShortNalIsAbsent();

private:
    // Build a baseline H.264 SPS NAL (profile_idc=66) whose VUI optionally
    // carries a colour_description. Minimal exp-Golomb encoder inline.
    static QByteArray makeSps(bool withColour, int primaries, int transfer,
                              int matrix, bool fullRange);
};

namespace {
struct BitWriter {
    QByteArray bytes;
    int bitPos = 0; // bits used in the current (last) byte
    void putBit(int b) {
        if (bitPos == 0) bytes.append(char(0));
        if (b) bytes[bytes.size() - 1] = char(quint8(bytes.back()) | (1 << (7 - bitPos)));
        bitPos = (bitPos + 1) & 7;
    }
    void putBits(unsigned v, int n) {
        for (int i = n - 1; i >= 0; --i) putBit((v >> i) & 1);
    }
    void ue(unsigned v) {
        unsigned x = v + 1;
        int n = 0;
        while (x >> (n + 1)) ++n;          // floor(log2(x))
        for (int i = 0; i < n; ++i) putBit(0);
        for (int i = n; i >= 0; --i) putBit((x >> i) & 1);
    }
    void se(int v) { ue(v <= 0 ? unsigned(-2 * v) : unsigned(2 * v - 1)); }
};
} // namespace

QByteArray TestColorVui::makeSps(bool withColour, int primaries, int transfer,
                                 int matrix, bool fullRange) {
    BitWriter w;
    // NAL header: forbidden_zero(0) nal_ref_idc(3) nal_unit_type(7=SPS).
    w.putBits(0x67, 8);
    w.putBits(66, 8);   // profile_idc = baseline
    w.putBits(0, 8);    // constraint flags + reserved
    w.putBits(31, 8);   // level_idc = 3.1
    w.ue(0);            // seq_parameter_set_id
    w.ue(0);            // log2_max_frame_num_minus4
    w.ue(0);            // pic_order_cnt_type
    w.ue(0);            // log2_max_pic_order_cnt_lsb_minus4
    w.ue(1);            // max_num_ref_frames
    w.putBit(0);        // gaps_in_frame_num_value_allowed_flag
    w.ue(7);            // pic_width_in_mbs_minus1  (128px)
    w.ue(7);            // pic_height_in_map_units_minus1 (128px)
    w.putBit(1);        // frame_mbs_only_flag
    w.putBit(0);        // direct_8x8_inference_flag
    w.putBit(0);        // frame_cropping_flag
    w.putBit(1);        // vui_parameters_present_flag
    // --- VUI ---
    w.putBit(0);        // aspect_ratio_info_present_flag
    w.putBit(0);        // overscan_info_present_flag
    if (withColour) {
        w.putBit(1);            // video_signal_type_present_flag
        w.putBits(5, 3);        // video_format = unspecified
        w.putBit(fullRange ? 1 : 0); // video_full_range_flag
        w.putBit(1);            // colour_description_present_flag
        w.putBits(unsigned(primaries), 8);
        w.putBits(unsigned(transfer), 8);
        w.putBits(unsigned(matrix), 8);
    } else {
        w.putBit(0);            // video_signal_type_present_flag
    }
    w.putBit(0);        // chroma_loc_info_present_flag
    w.putBit(0);        // timing_info_present_flag
    w.putBit(0);        // nal_hrd_parameters_present_flag
    w.putBit(0);        // vcl_hrd_parameters_present_flag
    w.putBit(0);        // pic_struct_present_flag
    w.putBit(0);        // bitstream_restriction_flag
    w.putBit(1);        // rbsp_stop_one_bit
    return w.bytes;
}

void TestColorVui::noVuiYieldsAbsent() {
    const QByteArray sps = makeSps(/*withColour=*/false, 0, 0, 0, false);
    const VuiColorInfo info = parseSpsColorVui(NativeVideoCodec::H264, sps);
    QVERIFY(!info.present);
}

void TestColorVui::bt709FullRangeIsParsed() {
    // primaries=1 (BT.709), transfer=1 (BT.709), matrix=1 (BT.709), full range.
    const QByteArray sps = makeSps(/*withColour=*/true, 1, 1, 1, /*fullRange=*/true);
    const VuiColorInfo info = parseSpsColorVui(NativeVideoCodec::H264, sps);
    QVERIFY(info.present);
    QCOMPARE(int(info.primaries), int(ColorPrimaries::Bt709));
    QCOMPARE(int(info.transfer), int(ColorTransfer::Bt709));
    QCOMPARE(int(info.matrix), int(ColorMatrix::Bt709));
    QCOMPARE(int(info.range), int(ColorRange::Full));
}

void TestColorVui::bt601VideoRangeIsParsed() {
    // primaries=6 (BT.601 525), transfer=6 (BT.601), matrix=6 (BT.601), video range.
    const QByteArray sps = makeSps(/*withColour=*/true, 6, 6, 6, /*fullRange=*/false);
    const VuiColorInfo info = parseSpsColorVui(NativeVideoCodec::H264, sps);
    QVERIFY(info.present);
    QCOMPARE(int(info.primaries), int(ColorPrimaries::Bt601));
    QCOMPARE(int(info.transfer), int(ColorTransfer::Bt601));
    QCOMPARE(int(info.matrix), int(ColorMatrix::Bt601));
    QCOMPARE(int(info.range), int(ColorRange::Video));
}

void TestColorVui::tooShortNalIsAbsent() {
    const QByteArray sps = QByteArrayLiteral("\x67\x42");
    const VuiColorInfo info = parseSpsColorVui(NativeVideoCodec::H264, sps);
    QVERIFY(!info.present);
}

QTEST_GUILESS_MAIN(TestColorVui)
#include "tst_colorvui.moc"
```

Register in `tests/unit/CMakeLists.txt` immediately after the `tst_h26xaccessunit` line:

```cmake
olr_add_unit_test(tst_colorvui olr_test_core)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build/c --target tst_colorvui`
Expected: FAIL to compile — `recorder_engine/ingest/colorvui.h: No such file or directory`.

- [ ] **Step 3: Write the minimal implementation**

Create `recorder_engine/ingest/colorvui.h`:

```cpp
#ifndef OLR_COLORVUI_H
#define OLR_COLORVUI_H

#include "playback/output/colormetadata.h"
#include "recorder_engine/ingest/pespacket.h" // NativeVideoCodec

#include <QByteArray>

// Colour fields extracted from an SPS VUI's colour_description. `present` is
// false when no colour_description was signalled (the caller then falls back to
// the default-tagging policy in colormetadatapolicy.h).
struct VuiColorInfo {
    bool present = false;
    ColorRange range = ColorRange::Video;
    ColorPrimaries primaries = ColorPrimaries::Unspecified;
    ColorTransfer transfer = ColorTransfer::Unspecified;
    ColorMatrix matrix = ColorMatrix::Bt709;
};

// Parse the VUI colour fields from a raw SPS NAL payload (NAL header byte
// first; no start code, no length prefix). Supports H.264 SPS (nal_unit_type
// 7). Returns {present=false} for unsupported codecs, short NALs, or a VUI
// without a colour_description. Never reads past nal.size().
VuiColorInfo parseSpsColorVui(NativeVideoCodec codec, const QByteArray& nal);

#endif // OLR_COLORVUI_H
```

Create `recorder_engine/ingest/colorvui.cpp`:

```cpp
#include "recorder_engine/ingest/colorvui.h"

namespace {

// Reads an RBSP bitstream with emulation-prevention (0x000003 -> 0x0000)
// removal, starting after the 1-byte NAL header.
class RbspReader {
public:
    RbspReader(const quint8* data, int size, int startByte)
        : m_data(data), m_size(size), m_byte(startByte) {}

    bool eof() const { return m_byte >= m_size; }

    int bit() {
        if (m_byte >= m_size) {
            m_overrun = true;
            return 0;
        }
        // Emulation prevention: a 0x03 after two 0x00 bytes is skipped.
        if (m_zeroes >= 2 && m_data[m_byte] == 0x03 && m_bit == 0) {
            ++m_byte;
            m_zeroes = 0;
            if (m_byte >= m_size) {
                m_overrun = true;
                return 0;
            }
        }
        const int b = (m_data[m_byte] >> (7 - m_bit)) & 1;
        if (m_bit == 0) {
            if (m_data[m_byte] == 0x00) ++m_zeroes;
            else m_zeroes = 0;
        }
        if (++m_bit == 8) {
            m_bit = 0;
            ++m_byte;
        }
        return b;
    }

    unsigned bits(int n) {
        unsigned v = 0;
        for (int i = 0; i < n; ++i) v = (v << 1) | unsigned(bit());
        return v;
    }

    unsigned ue() {
        int zeros = 0;
        while (!m_overrun && bit() == 0 && zeros < 32) ++zeros;
        return (1u << zeros) - 1 + bits(zeros);
    }

    int se() {
        const unsigned k = ue();
        const int sign = (k & 1) ? 1 : -1;
        return sign * int((k + 1) / 2);
    }

    bool overrun() const { return m_overrun; }

private:
    const quint8* m_data;
    int m_size;
    int m_byte;
    int m_bit = 0;
    int m_zeroes = 0;
    bool m_overrun = false;
};

ColorPrimaries mapPrimaries(unsigned code) {
    switch (code) {
    case 1: return ColorPrimaries::Bt709;
    case 5:
    case 6: return ColorPrimaries::Bt601;
    case 9: return ColorPrimaries::Bt2020;
    default: return ColorPrimaries::Unspecified;
    }
}

ColorTransfer mapTransfer(unsigned code) {
    switch (code) {
    case 1:
    case 6:
    case 14:
    case 15: return ColorTransfer::Bt709;
    case 5:
    case 8: return ColorTransfer::Bt601;
    case 16: return ColorTransfer::Bt2020;
    default: return ColorTransfer::Unspecified;
    }
}

ColorMatrix mapMatrix(unsigned code) {
    switch (code) {
    case 5:
    case 6: return ColorMatrix::Bt601;
    case 9:
    case 10: return ColorMatrix::Bt2020;
    default: return ColorMatrix::Bt709; // 1 (BT.709) and unspecified -> Bt709
    }
}

} // namespace

VuiColorInfo parseSpsColorVui(NativeVideoCodec codec, const QByteArray& nal) {
    VuiColorInfo out;
    if (codec != NativeVideoCodec::H264) return out;
    if (nal.size() < 4) return out;

    const auto* data = reinterpret_cast<const quint8*>(nal.constData());
    const int nalType = data[0] & 0x1f;
    if (nalType != 7) return out; // not an SPS

    RbspReader r(data, nal.size(), 1); // skip the NAL header byte

    const unsigned profileIdc = r.bits(8);
    r.bits(8);  // constraint flags + reserved
    r.bits(8);  // level_idc
    r.ue();     // seq_parameter_set_id

    // High-profile chroma/bit-depth block (skipped for the common 8-bit 4:2:0).
    if (profileIdc == 100 || profileIdc == 110 || profileIdc == 122 ||
        profileIdc == 244 || profileIdc == 44 || profileIdc == 83 ||
        profileIdc == 86 || profileIdc == 118 || profileIdc == 128 ||
        profileIdc == 138 || profileIdc == 139 || profileIdc == 134) {
        const unsigned chromaFormatIdc = r.ue();
        if (chromaFormatIdc == 3) r.bit(); // separate_colour_plane_flag
        r.ue();                            // bit_depth_luma_minus8
        r.ue();                            // bit_depth_chroma_minus8
        r.bit();                           // qpprime_y_zero_transform_bypass_flag
        if (r.bit()) {                     // seq_scaling_matrix_present_flag
            // Defensive: refuse to walk scaling lists (rare in our recordings);
            // a present scaling matrix means the VUI offset is unknown to us.
            return out;
        }
    }

    r.ue(); // log2_max_frame_num_minus4
    const unsigned picOrderCntType = r.ue();
    if (picOrderCntType == 0) {
        r.ue(); // log2_max_pic_order_cnt_lsb_minus4
    } else if (picOrderCntType == 1) {
        r.bit();                                   // delta_pic_order_always_zero_flag
        r.se();                                    // offset_for_non_ref_pic
        r.se();                                    // offset_for_top_to_bottom_field
        const unsigned n = r.ue();                 // num_ref_frames_in_pic_order_cnt_cycle
        for (unsigned i = 0; i < n && !r.overrun(); ++i) r.se();
    }
    r.ue();  // max_num_ref_frames
    r.bit(); // gaps_in_frame_num_value_allowed_flag
    r.ue();  // pic_width_in_mbs_minus1
    r.ue();  // pic_height_in_map_units_minus1
    const unsigned frameMbsOnly = r.bit();
    if (!frameMbsOnly) r.bit(); // mb_adaptive_frame_field_flag
    r.bit();                    // direct_8x8_inference_flag
    if (r.bit()) {              // frame_cropping_flag
        r.ue();
        r.ue();
        r.ue();
        r.ue();
    }
    const unsigned vuiPresent = r.bit();
    if (!vuiPresent || r.overrun()) return out;

    if (r.bit()) {              // aspect_ratio_info_present_flag
        const unsigned aspect = r.bits(8);
        if (aspect == 255) {    // Extended_SAR
            r.bits(16);
            r.bits(16);
        }
    }
    if (r.bit()) r.bit();       // overscan_info_present_flag -> overscan_appropriate_flag

    if (r.bit()) {              // video_signal_type_present_flag
        r.bits(3);              // video_format
        out.range = r.bit() ? ColorRange::Full : ColorRange::Video; // video_full_range_flag
        if (r.bit()) {          // colour_description_present_flag
            const unsigned primaries = r.bits(8);
            const unsigned transfer = r.bits(8);
            const unsigned matrix = r.bits(8);
            if (!r.overrun()) {
                out.present = true;
                out.primaries = mapPrimaries(primaries);
                out.transfer = mapTransfer(transfer);
                out.matrix = mapMatrix(matrix);
            }
        }
    }
    return out;
}
```

Add the source to the engine/core library in `CMakeLists.txt`, alongside the existing `recorder_engine/ingest/h26xaccessunit.{h,cpp}` entry (search for `h26xaccessunit.cpp` in the `olr_engine` / `olr_test_core` source list and add the pair on the next line):

```cmake
        recorder_engine/ingest/colorvui.h recorder_engine/ingest/colorvui.cpp
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build build/c --target tst_colorvui && ctest --test-dir build/c -R tst_colorvui --output-on-failure`
Expected: PASS (4 tests).

- [ ] **Step 5: Commit**

```bash
git add recorder_engine/ingest/colorvui.h recorder_engine/ingest/colorvui.cpp \
        tests/unit/tst_colorvui.cpp tests/unit/CMakeLists.txt CMakeLists.txt
git commit -m "feat(color): SPS-VUI colour parser (H.264) -> ColorMetadata enums"
```

---

## Task 2: Default-tagging policy (the Phase-0 no-op calibration)

The single function that decides a frame's `ColorMetadata` from (a) a parsed VUI, (b) FFmpeg `AVFrame` color fields, or (c) — when both are absent/unspecified — the **height-based default that reproduces today's `height>576 → BT709/BT601 + Video range`**. This is the calibration the whole "provable no-op" gate rests on: untagged fixtures must come out exactly as the old `qtpreviewsink` heuristic produced.

**Files:**
- Create: `playback/output/colormetadatapolicy.h`, `playback/output/colormetadatapolicy.cpp`
- Test: `tests/unit/tst_colormetadatapolicy.cpp`
- Modify: `CMakeLists.txt` (add to the playback library source list), `tests/unit/CMakeLists.txt` (register `tst_colormetadatapolicy olr_test_playback`)

**Interfaces:**
- Consumes: `ColorMetadata` and its enums (keystone, `playback/output/colormetadata.h`); `VuiColorInfo` (Task 1); FFmpeg `AVColorSpace`/`AVColorRange`/`AVColorPrimaries`/`AVColorTransferCharacteristic` integer codes (passed as `int`, **not** the FFmpeg headers — keeps this header FFmpeg-free).
- Produces:
  ```cpp
  // playback/output/colormetadatapolicy.h
  // The height threshold that historically selected BT.709 (>576) vs BT.601.
  // Matches the retired qtpreviewsink.cpp:23 heuristic exactly.
  constexpr int kDefaultBt709HeightThreshold = 576;

  // Height-only default: reproduces the legacy height>576 -> BT709 / else BT601
  // mapping, with Video range and 8-bit 4:2:0 (the structural pipeline format).
  ColorMetadata defaultColorMetadataForHeight(int height);

  // Resolve final ColorMetadata for a decoded frame. Precedence:
  //   1. an explicit VUI colour_description (info.present), then
  //   2. explicit FFmpeg AVFrame color codes (avColorspace/Range/Primaries/Transfer
  //      where each is not AVCOL_*_UNSPECIFIED == 2), then
  //   3. the height-based legacy default.
  // Any field left Unspecified by 1/2 is filled from the height default so the
  // result is always fully populated. Pass info = {} and the AV codes as 2
  // (UNSPECIFIED) for the no-tag path -> equals defaultColorMetadataForHeight.
  ColorMetadata resolveColorMetadata(const VuiColorInfo& info, int height,
                                     int avColorspace, int avColorRange,
                                     int avColorPrimaries, int avColorTransfer);
  ```

> **No-op anchor.** Today every fixture is untagged (no SPS-VUI colour, FFmpeg yields `UNSPECIFIED`), and `qtpreviewsink` chose `height>576 ? BT709 : BT601` with `ColorRange_Video`. So with `info={}` and AV codes `=2`, `resolveColorMetadata` MUST return `{matrix: height>576?Bt709:Bt601, range: Video, primaries: matching, transfer: matching, chromaFormat: Yuv420, bitDepth: 8}`. The sink (Task 5) maps that back to exactly the old `ColorSpace_BT709/BT601 + ColorRange_Video`, so goldens are unchanged.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_colormetadatapolicy.cpp`:

```cpp
// Unit tests for the default-tagging policy. The load-bearing assertion is the
// no-op: an untagged frame (no VUI, FFmpeg UNSPECIFIED) reproduces the legacy
// height>576 -> BT.709 / else BT.601 + Video-range mapping exactly, so existing
// goldens are unchanged.
#include <QtTest>

#include "playback/output/colormetadatapolicy.h"
#include "recorder_engine/ingest/colorvui.h"

// FFmpeg's AVCOL_*_UNSPECIFIED is the integer 2; we pass codes as ints.
static constexpr int kUnspecified = 2;

class TestColorMetadataPolicy : public QObject {
    Q_OBJECT
private slots:
    void untaggedTallFrameIsBt709VideoNoOp();
    void untaggedShortFrameIsBt601VideoNoOp();
    void vuiOverridesHeightDefault();
    void ffmpegCodesOverrideHeightDefaultWhenNoVui();
    void heightThresholdMatchesLegacyConstant();
};

void TestColorMetadataPolicy::heightThresholdMatchesLegacyConstant() {
    QCOMPARE(kDefaultBt709HeightThreshold, 576);
}

void TestColorMetadataPolicy::untaggedTallFrameIsBt709VideoNoOp() {
    const ColorMetadata m = resolveColorMetadata(VuiColorInfo{}, /*height=*/1080,
                                                 kUnspecified, kUnspecified,
                                                 kUnspecified, kUnspecified);
    QCOMPARE(int(m.matrix), int(ColorMatrix::Bt709));
    QCOMPARE(int(m.range), int(ColorRange::Video));
    QCOMPARE(int(m.primaries), int(ColorPrimaries::Bt709));
    QCOMPARE(int(m.transfer), int(ColorTransfer::Bt709));
    QCOMPARE(int(m.chromaFormat), int(ChromaFormat::Yuv420));
    QCOMPARE(m.bitDepth, 8);
    // Exact-equal to the height-only default (the no-op anchor).
    QVERIFY(m == defaultColorMetadataForHeight(1080));
}

void TestColorMetadataPolicy::untaggedShortFrameIsBt601VideoNoOp() {
    const ColorMetadata m = resolveColorMetadata(VuiColorInfo{}, /*height=*/480,
                                                 kUnspecified, kUnspecified,
                                                 kUnspecified, kUnspecified);
    QCOMPARE(int(m.matrix), int(ColorMatrix::Bt601));
    QCOMPARE(int(m.range), int(ColorRange::Video));
    QCOMPARE(int(m.primaries), int(ColorPrimaries::Bt601));
    QCOMPARE(int(m.transfer), int(ColorTransfer::Bt601));
    QVERIFY(m == defaultColorMetadataForHeight(480));
}

void TestColorMetadataPolicy::vuiOverridesHeightDefault() {
    // A short frame explicitly tagged BT.709 full-range must NOT be downgraded
    // to the height default.
    VuiColorInfo vui;
    vui.present = true;
    vui.range = ColorRange::Full;
    vui.primaries = ColorPrimaries::Bt709;
    vui.transfer = ColorTransfer::Bt709;
    vui.matrix = ColorMatrix::Bt709;
    const ColorMetadata m = resolveColorMetadata(vui, /*height=*/480,
                                                 kUnspecified, kUnspecified,
                                                 kUnspecified, kUnspecified);
    QCOMPARE(int(m.matrix), int(ColorMatrix::Bt709));
    QCOMPARE(int(m.range), int(ColorRange::Full));
    QCOMPARE(int(m.primaries), int(ColorPrimaries::Bt709));
}

void TestColorMetadataPolicy::ffmpegCodesOverrideHeightDefaultWhenNoVui() {
    // No VUI, but FFmpeg signals BT.601 (AVCOL_SPC_SMPTE170M == 6,
    // AVCOL_PRI_SMPTE170M == 6, AVCOL_TRC_SMPTE170M == 6) on a 1080 frame.
    const ColorMetadata m = resolveColorMetadata(VuiColorInfo{}, /*height=*/1080,
                                                 /*colorspace=*/6, /*range=*/1,
                                                 /*primaries=*/6, /*transfer=*/6);
    QCOMPARE(int(m.matrix), int(ColorMatrix::Bt601));
    QCOMPARE(int(m.primaries), int(ColorPrimaries::Bt601));
    QCOMPARE(int(m.transfer), int(ColorTransfer::Bt601));
    QCOMPARE(int(m.range), int(ColorRange::Video)); // AVCOL_RANGE_MPEG == 1 -> Video
}

QTEST_GUILESS_MAIN(TestColorMetadataPolicy)
#include "tst_colormetadatapolicy.moc"
```

Register in `tests/unit/CMakeLists.txt` immediately after the `tst_qtpreviewsink` line:

```cmake
olr_add_unit_test(tst_colormetadatapolicy olr_test_playback)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build/c --target tst_colormetadatapolicy`
Expected: FAIL to compile — `playback/output/colormetadatapolicy.h: No such file or directory`.

- [ ] **Step 3: Write the minimal implementation**

Create `playback/output/colormetadatapolicy.h`:

```cpp
#ifndef COLORMETADATAPOLICY_H
#define COLORMETADATAPOLICY_H

#include "playback/output/colormetadata.h"

struct VuiColorInfo; // recorder_engine/ingest/colorvui.h

// The legacy threshold: frames taller than this defaulted to BT.709, shorter to
// BT.601 (the retired qtpreviewsink.cpp:23 heuristic).
constexpr int kDefaultBt709HeightThreshold = 576;

// Height-only legacy default: height>576 -> BT.709 else BT.601, Video range,
// 8-bit 4:2:0.
ColorMetadata defaultColorMetadataForHeight(int height);

// Resolve final ColorMetadata: explicit VUI first, then explicit FFmpeg AVFrame
// color codes (each UNSPECIFIED == 2 is ignored), then the height default for
// any still-unspecified field. avColorRange follows FFmpeg AVColorRange
// (AVCOL_RANGE_JPEG == 2 -> Full, AVCOL_RANGE_MPEG == 1 -> Video, 0 unspecified).
ColorMetadata resolveColorMetadata(const VuiColorInfo& info, int height,
                                   int avColorspace, int avColorRange,
                                   int avColorPrimaries, int avColorTransfer);

#endif // COLORMETADATAPOLICY_H
```

Create `playback/output/colormetadatapolicy.cpp`:

```cpp
#include "playback/output/colormetadatapolicy.h"

#include "recorder_engine/ingest/colorvui.h"

namespace {

// FFmpeg AVColorSpace integer codes (subset we recognise).
ColorMatrix matrixFromAv(int code) {
    switch (code) {
    case 1: return ColorMatrix::Bt709;        // AVCOL_SPC_BT709
    case 5:                                   // AVCOL_SPC_BT470BG
    case 6: return ColorMatrix::Bt601;        // AVCOL_SPC_SMPTE170M
    case 9:                                   // AVCOL_SPC_BT2020_NCL
    case 10: return ColorMatrix::Bt2020;      // AVCOL_SPC_BT2020_CL
    default: return ColorMatrix::Bt709;
    }
}

ColorPrimaries primariesFromAv(int code) {
    switch (code) {
    case 1: return ColorPrimaries::Bt709;     // AVCOL_PRI_BT709
    case 5:                                   // AVCOL_PRI_BT470BG
    case 6: return ColorPrimaries::Bt601;     // AVCOL_PRI_SMPTE170M
    case 9: return ColorPrimaries::Bt2020;    // AVCOL_PRI_BT2020
    default: return ColorPrimaries::Unspecified;
    }
}

ColorTransfer transferFromAv(int code) {
    switch (code) {
    case 1:                                   // AVCOL_TRC_BT709
    case 6:                                   // AVCOL_TRC_SMPTE170M
    case 14:                                  // AVCOL_TRC_BT2020_10
    case 15: return ColorTransfer::Bt709;     // AVCOL_TRC_BT2020_12 (HD-ish)
    case 5:                                   // AVCOL_TRC_GAMMA28
    case 8: return ColorTransfer::Bt601;      // AVCOL_TRC_LINEAR proxy for SD
    case 16: return ColorTransfer::Bt2020;    // AVCOL_TRC_SMPTE2084
    default: return ColorTransfer::Unspecified;
    }
}

} // namespace

ColorMetadata defaultColorMetadataForHeight(int height) {
    ColorMetadata m;
    const bool hd = height > kDefaultBt709HeightThreshold;
    m.matrix = hd ? ColorMatrix::Bt709 : ColorMatrix::Bt601;
    m.primaries = hd ? ColorPrimaries::Bt709 : ColorPrimaries::Bt601;
    m.transfer = hd ? ColorTransfer::Bt709 : ColorTransfer::Bt601;
    m.range = ColorRange::Video;
    m.chromaFormat = ChromaFormat::Yuv420;
    m.bitDepth = 8;
    return m;
}

ColorMetadata resolveColorMetadata(const VuiColorInfo& info, int height,
                                   int avColorspace, int avColorRange,
                                   int avColorPrimaries, int avColorTransfer) {
    ColorMetadata m = defaultColorMetadataForHeight(height);

    // Tier 2: explicit FFmpeg codes (each UNSPECIFIED == 2 is ignored).
    if (avColorspace != 2) m.matrix = matrixFromAv(avColorspace);
    if (avColorPrimaries != 2) {
        const ColorPrimaries p = primariesFromAv(avColorPrimaries);
        if (p != ColorPrimaries::Unspecified) m.primaries = p;
    }
    if (avColorTransfer != 2) {
        const ColorTransfer t = transferFromAv(avColorTransfer);
        if (t != ColorTransfer::Unspecified) m.transfer = t;
    }
    if (avColorRange == 2) m.range = ColorRange::Full;   // AVCOL_RANGE_JPEG
    else if (avColorRange == 1) m.range = ColorRange::Video; // AVCOL_RANGE_MPEG

    // Tier 1: an explicit VUI colour_description wins over everything above.
    if (info.present) {
        m.matrix = info.matrix;
        m.range = info.range;
        if (info.primaries != ColorPrimaries::Unspecified) m.primaries = info.primaries;
        if (info.transfer != ColorTransfer::Unspecified) m.transfer = info.transfer;
    }
    return m;
}
```

Add the source to the **playback** library source list in `CMakeLists.txt` (search for an existing `playback/output/qtpreviewsink.cpp` entry in the `olr_playback` / `olr_test_playback` source list and add the pair on the next line):

```cmake
        playback/output/colormetadatapolicy.h playback/output/colormetadatapolicy.cpp
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build build/c --target tst_colormetadatapolicy && ctest --test-dir build/c -R tst_colormetadatapolicy --output-on-failure`
Expected: PASS (5 tests).

- [ ] **Step 5: Commit**

```bash
git add playback/output/colormetadatapolicy.h playback/output/colormetadatapolicy.cpp \
        tests/unit/tst_colormetadatapolicy.cpp tests/unit/CMakeLists.txt CMakeLists.txt
git commit -m "feat(color): default-tagging policy reproducing the legacy height>576 mapping"
```

---

## Task 3: Apple `CMFormatDescription` colour extraction (platform-gated)

VideoToolbox carries the source colour tags on the `CMFormatDescription`. Pull them into a `VuiColorInfo` so the native decode path matches FFmpeg's `AVFrame` color fields. Pure mapping from `CFString` attachment values; gated to Apple. On non-Apple this function is not compiled and the FFmpeg/VUI tiers in Task 2 already cover the fallback.

**Files:**
- Create: `recorder_engine/ingest/colortags_apple.h`, `recorder_engine/ingest/colortags_apple.mm`
- Test: `tests/unit/tst_colortags_apple.cpp` (APPLE-only registration)
- Modify: `CMakeLists.txt` (compile the `.mm` into the engine app on APPLE), `tests/unit/CMakeLists.txt` (APPLE-gated `olr_test_colortags_apple` static lib + test)

**Interfaces:**
- Consumes: `VuiColorInfo` (Task 1); `ColorMatrix`/`ColorRange`/`ColorPrimaries`/`ColorTransfer` (keystone).
- Produces:
  ```cpp
  // recorder_engine/ingest/colortags_apple.h  (Apple only)
  #ifdef __APPLE__
  #include "recorder_engine/ingest/colorvui.h"
  #include <CoreMedia/CoreMedia.h>
  // Read the colour attachments (YCbCrMatrix, ColorPrimaries, TransferFunction,
  // FullRange) off a CMFormatDescription into a VuiColorInfo. Returns
  // {present=false} when fmt is null or carries no recognised colour attachment.
  VuiColorInfo colorVuiFromFormatDescription(CMFormatDescriptionRef fmt);
  #endif
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_colortags_apple.cpp`:

```cpp
// Unit tests for colorVuiFromFormatDescription — mapping CoreMedia colour
// attachments to VuiColorInfo. Apple-only; builds a synthetic
// CMFormatDescription with explicit colour attachments.
#include <QtTest>

#ifdef __APPLE__

#include "recorder_engine/ingest/colortags_apple.h"

#include <CoreMedia/CoreMedia.h>

class TestColorTagsApple : public QObject {
    Q_OBJECT
private slots:
    void bt709FullRangeAttachmentsAreRead();
    void nullFormatIsAbsent();

private:
    static CMVideoFormatDescriptionRef makeFmt(CFStringRef matrix, CFStringRef primaries,
                                               CFStringRef transfer, bool fullRange);
};

CMVideoFormatDescriptionRef TestColorTagsApple::makeFmt(CFStringRef matrix,
                                                        CFStringRef primaries,
                                                        CFStringRef transfer,
                                                        bool fullRange) {
    const void* keys[] = {kCMFormatDescriptionExtension_YCbCrMatrix,
                          kCMFormatDescriptionExtension_ColorPrimaries,
                          kCMFormatDescriptionExtension_TransferFunction,
                          kCMFormatDescriptionExtension_FullRangeVideo};
    const void* vals[] = {matrix, primaries, transfer,
                          fullRange ? kCFBooleanTrue : kCFBooleanFalse};
    CFDictionaryRef ext = CFDictionaryCreate(kCFAllocatorDefault, keys, vals, 4,
                                             &kCFTypeDictionaryKeyCallBacks,
                                             &kCFTypeDictionaryValueCallBacks);
    CMVideoFormatDescriptionRef fmt = nullptr;
    CMVideoFormatDescriptionCreate(kCFAllocatorDefault, kCMVideoCodecType_H264,
                                   1920, 1080, ext, &fmt);
    if (ext) CFRelease(ext);
    return fmt;
}

void TestColorTagsApple::bt709FullRangeAttachmentsAreRead() {
    CMVideoFormatDescriptionRef fmt = makeFmt(kCMFormatDescriptionYCbCrMatrix_ITU_R_709_2,
                                              kCMFormatDescriptionColorPrimaries_ITU_R_709_2,
                                              kCMFormatDescriptionTransferFunction_ITU_R_709_2,
                                              /*fullRange=*/true);
    QVERIFY(fmt != nullptr);
    const VuiColorInfo info = colorVuiFromFormatDescription(fmt);
    CFRelease(fmt);

    QVERIFY(info.present);
    QCOMPARE(int(info.matrix), int(ColorMatrix::Bt709));
    QCOMPARE(int(info.primaries), int(ColorPrimaries::Bt709));
    QCOMPARE(int(info.transfer), int(ColorTransfer::Bt709));
    QCOMPARE(int(info.range), int(ColorRange::Full));
}

void TestColorTagsApple::nullFormatIsAbsent() {
    const VuiColorInfo info = colorVuiFromFormatDescription(nullptr);
    QVERIFY(!info.present);
}

QTEST_GUILESS_MAIN(TestColorTagsApple)
#include "tst_colortags_apple.moc"

#else
QTEST_GUILESS_MAIN(TestColorTagsApple)  // never reached; Apple-only registration
class TestColorTagsApple : public QObject { Q_OBJECT };
#include "tst_colortags_apple.moc"
#endif
```

Register in `tests/unit/CMakeLists.txt` immediately after the `tst_colorvui` line, APPLE-gated and mirroring the `olr_test_nativevideodecoder` static-lib pattern (so the `.mm` links with the CoreMedia frameworks):

```cmake
if(APPLE)
    olr_add_unit_test(tst_colortags_apple olr_test_core)
    add_library(olr_test_colortags_apple STATIC
        "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/colortags_apple.mm")
    target_include_directories(olr_test_colortags_apple PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(olr_test_colortags_apple
        PUBLIC Qt6::Core olr_test_core "-framework CoreMedia" "-framework CoreFoundation")
    target_link_libraries(tst_colortags_apple PRIVATE olr_test_colortags_apple)
endif()
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build/c --target tst_colortags_apple`
Expected: FAIL — `recorder_engine/ingest/colortags_apple.h: No such file or directory`.

- [ ] **Step 3: Write the minimal implementation**

Create `recorder_engine/ingest/colortags_apple.h`:

```cpp
#ifndef OLR_COLORTAGS_APPLE_H
#define OLR_COLORTAGS_APPLE_H

#ifdef __APPLE__

#include "recorder_engine/ingest/colorvui.h"

#include <CoreMedia/CoreMedia.h>

// Read colour attachments (YCbCrMatrix / ColorPrimaries / TransferFunction /
// FullRangeVideo) off a CMFormatDescription into a VuiColorInfo. Returns
// {present=false} for a null description or one with no recognised colour tag.
VuiColorInfo colorVuiFromFormatDescription(CMFormatDescriptionRef fmt);

#endif // __APPLE__
#endif // OLR_COLORTAGS_APPLE_H
```

Create `recorder_engine/ingest/colortags_apple.mm`:

```objcpp
#include "recorder_engine/ingest/colortags_apple.h"

#ifdef __APPLE__

namespace {

CFStringRef attachmentString(CMFormatDescriptionRef fmt, CFStringRef key) {
    const void* v = CMFormatDescriptionGetExtension(fmt, key);
    return (v && CFGetTypeID(v) == CFStringGetTypeID()) ? CFStringRef(v) : nullptr;
}

ColorMatrix mapMatrix(CFStringRef s, bool* known) {
    *known = true;
    if (CFEqual(s, kCMFormatDescriptionYCbCrMatrix_ITU_R_709_2)) return ColorMatrix::Bt709;
    if (CFEqual(s, kCMFormatDescriptionYCbCrMatrix_ITU_R_601_4)) return ColorMatrix::Bt601;
    if (CFEqual(s, kCMFormatDescriptionYCbCrMatrix_SMPTE_240M_1995)) return ColorMatrix::Bt709;
    if (CFEqual(s, kCMFormatDescriptionYCbCrMatrix_ITU_R_2020)) return ColorMatrix::Bt2020;
    *known = false;
    return ColorMatrix::Bt709;
}

ColorPrimaries mapPrimaries(CFStringRef s, bool* known) {
    *known = true;
    if (CFEqual(s, kCMFormatDescriptionColorPrimaries_ITU_R_709_2)) return ColorPrimaries::Bt709;
    if (CFEqual(s, kCMFormatDescriptionColorPrimaries_SMPTE_C)) return ColorPrimaries::Bt601;
    if (CFEqual(s, kCMFormatDescriptionColorPrimaries_EBU_3213)) return ColorPrimaries::Bt601;
    if (CFEqual(s, kCMFormatDescriptionColorPrimaries_ITU_R_2020)) return ColorPrimaries::Bt2020;
    *known = false;
    return ColorPrimaries::Unspecified;
}

ColorTransfer mapTransfer(CFStringRef s, bool* known) {
    *known = true;
    if (CFEqual(s, kCMFormatDescriptionTransferFunction_ITU_R_709_2)) return ColorTransfer::Bt709;
    if (CFEqual(s, kCMFormatDescriptionTransferFunction_SMPTE_240M_1995)) return ColorTransfer::Bt709;
    if (CFEqual(s, kCMFormatDescriptionTransferFunction_ITU_R_2020)) return ColorTransfer::Bt2020;
    *known = false;
    return ColorTransfer::Unspecified;
}

} // namespace

VuiColorInfo colorVuiFromFormatDescription(CMFormatDescriptionRef fmt) {
    VuiColorInfo out;
    if (!fmt) return out;

    bool any = false;
    if (CFStringRef m = attachmentString(fmt, kCMFormatDescriptionExtension_YCbCrMatrix)) {
        bool known = false;
        const ColorMatrix v = mapMatrix(m, &known);
        if (known) { out.matrix = v; any = true; }
    }
    if (CFStringRef p = attachmentString(fmt, kCMFormatDescriptionExtension_ColorPrimaries)) {
        bool known = false;
        const ColorPrimaries v = mapPrimaries(p, &known);
        if (known) { out.primaries = v; any = true; }
    }
    if (CFStringRef t = attachmentString(fmt, kCMFormatDescriptionExtension_TransferFunction)) {
        bool known = false;
        const ColorTransfer v = mapTransfer(t, &known);
        if (known) { out.transfer = v; any = true; }
    }
    const void* fr = CMFormatDescriptionGetExtension(fmt, kCMFormatDescriptionExtension_FullRangeVideo);
    if (fr && CFGetTypeID(fr) == CFBooleanGetTypeID()) {
        out.range = CFBooleanGetValue(CFBooleanRef(fr)) ? ColorRange::Full : ColorRange::Video;
        any = true;
    }
    out.present = any;
    return out;
}

#endif // __APPLE__
```

Add the `.mm` to the **APPLE** branch of the engine app `target_sources` in `CMakeLists.txt`, alongside the existing `recorder_engine/ingest/nativevideodecoder_videotoolbox.mm` entry:

```cmake
        recorder_engine/ingest/colortags_apple.mm
```

- [ ] **Step 4: Run the test to verify it passes (macOS)**

Run: `cmake --build build/c --target tst_colortags_apple && ctest --test-dir build/c -R tst_colortags_apple --output-on-failure`
Expected: PASS (2 tests) on macOS.

- [ ] **Step 5: Commit**

```bash
git add recorder_engine/ingest/colortags_apple.h recorder_engine/ingest/colortags_apple.mm \
        tests/unit/tst_colortags_apple.cpp tests/unit/CMakeLists.txt CMakeLists.txt
git commit -m "feat(color): read CMFormatDescription colour attachments (Apple)"
```

---

## Task 4: Plumb `ColorMetadata` into `FrameMetadata` at the decode/convert edge

Populate `FrameHandle::metadata().color` for every decoded frame. The FFmpeg path reads the `AVFrame`'s color fields; the native path's parsed VUI / `CMFormatDescription` tags feed the same resolver. With today's untagged content this is a strict no-op — every frame resolves to the height default — but the field is now carried instead of being re-guessed at the sink.

**Files:**
- Modify: `playback/playbackworker.cpp` (the keystone-migrated `convertToMediaVideoFrame(AVFrame*, int)` now returning `FrameHandle`), `playback/playbackworker.h` if a helper signature is added
- Test: `tests/unit/tst_colormetadataplumb.cpp` (a focused test that exercises the resolver against an `AVFrame`'s color fields, independent of the worker's threading)

**Interfaces:**
- Consumes: `resolveColorMetadata` (Task 2), `FrameMetadata`/`FrameHandle`/`makeCpuFrameHandle` (keystone).
- Produces: a free helper so the mapping is unit-testable without standing up a `PlaybackWorker`:
  ```cpp
  // playback/playbackworker.h (declaration) / playbackworker.cpp (definition)
  // Resolve ColorMetadata for a decoded AVFrame using its explicit color fields
  // (colorspace/range/primaries/color_trc) and the height default fallback.
  ColorMetadata colorMetadataForAvFrame(const AVFrame* frame);
  ```
  After this task, `convertToMediaVideoFrame` sets `meta.color = colorMetadataForAvFrame(frame)` on the `FrameMetadata` it passes to `makeCpuFrameHandle`.

> The native VideoToolbox decode path produces a CPU `AVFrame` via `copyPixelBufferToAvFrame` ([nativevideodecoder_videotoolbox.mm:129](../../../recorder_engine/ingest/nativevideodecoder_videotoolbox.mm)), so it flows through the same `convertToMediaVideoFrame` ([playbackworker.cpp:621](../../../playback/playbackworker.cpp), [:694](../../../playback/playbackworker.cpp)). Wiring `CMFormatDescription` tags onto that `AVFrame` (so `colorMetadataForAvFrame` sees them) is a native-decoder enhancement deferred to `gpu-abstraction` (Phase 2), where the keep-surface import edge carries the format description. In Phase 1 the native path's `AVFrame` color fields are `UNSPECIFIED`, so it resolves to the same height default as the FFmpeg path — the no-op holds. Task 3's extractor ships now so the Phase-2 wiring has a tested mapping to call.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/tst_colormetadataplumb.cpp`:

```cpp
// Verifies colorMetadataForAvFrame: an untagged AVFrame resolves to the height
// default (the no-op), and an explicitly-tagged AVFrame is honoured.
#include <QtTest>

#include "playback/output/colormetadatapolicy.h"
#include "playback/playbackworker.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

class TestColorMetadataPlumb : public QObject {
    Q_OBJECT
private slots:
    void untaggedAvFrameResolvesToHeightDefault();
    void taggedAvFrameIsHonoured();

private:
    static AVFrame* makeFrame(int w, int h);
};

AVFrame* TestColorMetadataPlumb::makeFrame(int w, int h) {
    AVFrame* f = av_frame_alloc();
    f->format = AV_PIX_FMT_YUV420P;
    f->width = w;
    f->height = h;
    // Leave color fields at their alloc defaults (AVCOL_*_UNSPECIFIED).
    return f;
}

void TestColorMetadataPlumb::untaggedAvFrameResolvesToHeightDefault() {
    AVFrame* f = makeFrame(1920, 1080);
    const ColorMetadata m = colorMetadataForAvFrame(f);
    av_frame_free(&f);
    QVERIFY(m == defaultColorMetadataForHeight(1080)); // no-op anchor
}

void TestColorMetadataPlumb::taggedAvFrameIsHonoured() {
    AVFrame* f = makeFrame(720, 480);
    f->colorspace = AVCOL_SPC_BT709;
    f->color_primaries = AVCOL_PRI_BT709;
    f->color_trc = AVCOL_TRC_BT709;
    f->color_range = AVCOL_RANGE_JPEG; // full
    const ColorMetadata m = colorMetadataForAvFrame(f);
    av_frame_free(&f);
    QCOMPARE(int(m.matrix), int(ColorMatrix::Bt709));
    QCOMPARE(int(m.primaries), int(ColorPrimaries::Bt709));
    QCOMPARE(int(m.range), int(ColorRange::Full));
}

QTEST_GUILESS_MAIN(TestColorMetadataPlumb)
#include "tst_colormetadataplumb.moc"
```

Register in `tests/unit/CMakeLists.txt` immediately after the `tst_colormetadatapolicy` line:

```cmake
olr_add_unit_test(tst_colormetadataplumb olr_test_playback)
target_include_directories(tst_colormetadataplumb PRIVATE "${OLR_FFMPEG_INCLUDE}")
target_link_directories(tst_colormetadataplumb PRIVATE "${OLR_FFMPEG_LIBDIR}")
target_link_libraries(tst_colormetadataplumb PRIVATE avutil)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build/c --target tst_colormetadataplumb`
Expected: FAIL to link/compile — `colorMetadataForAvFrame` is undefined.

- [ ] **Step 3: Write the minimal implementation**

Declare the helper in `playback/playbackworker.h` near the existing `convertToMediaVideoFrame` declaration ([playbackworker.h:162](../../../playback/playbackworker.h)). It is a free function (not a member) so the test can call it without a worker:

```cpp
// Resolve ColorMetadata for a decoded AVFrame: explicit color fields when
// signalled, else the legacy height>576 default. Free function (no worker
// state) so the mapping is unit-testable in isolation.
ColorMetadata colorMetadataForAvFrame(const AVFrame* frame);
```

Add the include for the keystone color types at the top of `playback/playbackworker.h` if not already present (the keystone migration pulls in `framehandle.h`, which includes `colormetadata.h`; if compilation reports `ColorMetadata` undeclared, add `#include "playback/output/colormetadata.h"`).

Define it in `playback/playbackworker.cpp`, just above `PlaybackWorker::convertToMediaVideoFrame` ([playbackworker.cpp:2305](../../../playback/playbackworker.cpp)). Add `#include "playback/output/colormetadatapolicy.h"` to the file's includes:

```cpp
ColorMetadata colorMetadataForAvFrame(const AVFrame* frame) {
    if (!frame) return defaultColorMetadataForHeight(0);
    // No SPS-VUI at this edge (the AVFrame already carries resolved codes), so
    // pass an absent VuiColorInfo and let the AVFrame color fields drive tier 2.
    return resolveColorMetadata(VuiColorInfo{}, frame->height,
                                int(frame->colorspace), int(frame->color_range),
                                int(frame->color_primaries), int(frame->color_trc));
}
```

Add `#include "recorder_engine/ingest/colorvui.h"` to `playbackworker.cpp` for `VuiColorInfo`. Then, inside `convertToMediaVideoFrame`, set the resolved color on the `FrameMetadata` the keystone build passes to `makeCpuFrameHandle`. The keystone migration already constructs a `FrameMetadata meta;` here; add one line before the handle is built:

```cpp
    meta.color = colorMetadataForAvFrame(frame);
```

(If the keystone named the local differently, set `.color` on whichever `FrameMetadata` instance feeds `makeCpuFrameHandle`. Do not add a second metadata object.)

- [ ] **Step 4: Run the test + the full unit label to verify no regression**

Run: `cmake --build build/c --target tst_colormetadataplumb && ctest --test-dir build/c -R tst_colormetadataplumb --output-on-failure`
Expected: PASS (2 tests).

Then the full unit suite (a convert-path change can ripple into trackbuffer/outputframecache/outputbusengine tests):

Run: `cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure`
Expected: PASS — all existing tests with identical assertion values (the resolved color equals the height default for untagged content, and nothing reads `metadata().color` yet except the new tests).

- [ ] **Step 5: Commit**

```bash
git add playback/playbackworker.h playback/playbackworker.cpp \
        tests/unit/tst_colormetadataplumb.cpp tests/unit/CMakeLists.txt
git commit -m "feat(color): carry resolved ColorMetadata on every decoded FrameHandle"
```

---

## Task 5: Replace the qtpreviewsink height>576 heuristic + decode->sink round-trip gate

Switch `QtPreviewSink::toQVideoFrame` from inferring the color space by height to reading `handle.metadata().color`, mapping each `ColorMatrix`/`ColorRange` to its `QVideoFrameFormat` equivalent. Add the spec-mandated round-trip unit test asserting the metadata set at the decode edge arrives intact at the sink. Because the default-tagging policy reproduces the old `height>576` mapping, the produced `QVideoFrameFormat` color space is byte-identical for today's untagged content — the existing `tst_qtpreviewsink` and `e2e_play` goldens are unchanged.

**Files:**
- Modify: `playback/output/qtpreviewsink.cpp` (replace the `frame.height > 576` branch), `playback/output/qtpreviewsink.h` if a mapping helper is exposed
- Test: `tests/unit/tst_qtpreviewsink.cpp` (extend with the round-trip + mapping assertions)

**Interfaces:**
- Consumes: `FrameHandle`/`MediaVideoFrameView`/`ColorMetadata` (keystone). The keystone migrated `toQVideoFrame` to `static QVideoFrame toQVideoFrame(const FrameHandle& frame)` and `deliver(const FrameHandle&)`.
- Produces: a small pure mapping helper so the conversion is assertable without a `QVideoFrame::map`:
  ```cpp
  // playback/output/qtpreviewsink.h
  // Map keystone color enums to Qt's video-frame color descriptors. Bt2020 maps
  // to ColorSpace_BT2020 (carried for forward-compat; today's content is 601/709).
  QVideoFrameFormat::ColorSpace qtColorSpaceFor(ColorMatrix matrix);
  QVideoFrameFormat::ColorRange qtColorRangeFor(ColorRange range);
  ```

- [ ] **Step 1: Write the failing test**

Extend `tests/unit/tst_qtpreviewsink.cpp`. Add the include and slot declarations:

```cpp
#include "playback/output/colormetadatapolicy.h"
#include "playback/output/framehandle.h"
```

```cpp
    void colorMetadataRoundTripsDecodeToSink();
    void taggedBt601FrameMapsToBt601();
    void defaultTaggingReproducesLegacyHeightHeuristic();
```

Add the bodies. These assume the keystone helpers `solidYuv420pHandle(w,h,y,u,v)` and `FrameHandle::metadata()` (mutable) exist:

```cpp
void TestQtPreviewSink::colorMetadataRoundTripsDecodeToSink() {
    // "Decode edge": a 1080-tall handle tagged with the resolved default.
    FrameHandle handle = solidYuv420pHandle(1920, 1080, 80, 128, 128);
    handle.metadata().color = defaultColorMetadataForHeight(1080);

    const QVideoFrame qFrame = QtPreviewSink::toQVideoFrame(handle);
    QVERIFY(qFrame.isValid());
    // The metadata set at the edge survives to the sink's QVideoFrameFormat.
    QCOMPARE(qFrame.surfaceFormat().colorSpace(), QVideoFrameFormat::ColorSpace_BT709);
    QCOMPARE(qFrame.surfaceFormat().colorRange(), QVideoFrameFormat::ColorRange_Video);
}

void TestQtPreviewSink::taggedBt601FrameMapsToBt601() {
    // A *tall* frame explicitly tagged BT.601 must map to BT.601 — proving the
    // sink reads metadata, not height (the legacy code would have said BT.709).
    FrameHandle handle = solidYuv420pHandle(1920, 1080, 80, 128, 128);
    ColorMetadata c;
    c.matrix = ColorMatrix::Bt601;
    c.range = ColorRange::Video;
    handle.metadata().color = c;

    const QVideoFrame qFrame = QtPreviewSink::toQVideoFrame(handle);
    QVERIFY(qFrame.isValid());
    QCOMPARE(qFrame.surfaceFormat().colorSpace(), QVideoFrameFormat::ColorSpace_BT601);
}

void TestQtPreviewSink::defaultTaggingReproducesLegacyHeightHeuristic() {
    // No-op proof: the default policy at 480 and 1080 yields exactly what the
    // retired height>576 branch produced.
    FrameHandle tall = solidYuv420pHandle(1280, 720, 80, 128, 128);
    tall.metadata().color = defaultColorMetadataForHeight(720);
    QCOMPARE(QtPreviewSink::toQVideoFrame(tall).surfaceFormat().colorSpace(),
             QVideoFrameFormat::ColorSpace_BT709);

    FrameHandle shortFrame = solidYuv420pHandle(720, 480, 80, 128, 128);
    shortFrame.metadata().color = defaultColorMetadataForHeight(480);
    QCOMPARE(QtPreviewSink::toQVideoFrame(shortFrame).surfaceFormat().colorSpace(),
             QVideoFrameFormat::ColorSpace_BT601);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build/c --target tst_qtpreviewsink && ctest --test-dir build/c -R tst_qtpreviewsink --output-on-failure`
Expected: FAIL — `qtColorSpaceFor` undefined (if the mapping helpers are referenced), or `taggedBt601FrameMapsToBt601` FAILS because the current code still infers BT.709 from height (1080 > 576) and ignores the BT.601 metadata.

- [ ] **Step 3: Write the minimal implementation**

In `playback/output/qtpreviewsink.h`, add the mapping helper declarations (after the includes; `ColorMetadata`/`ColorMatrix`/`ColorRange` come via the keystone `framehandle.h` the migrated header already includes — if not, add `#include "playback/output/colormetadata.h"`):

```cpp
QVideoFrameFormat::ColorSpace qtColorSpaceFor(ColorMatrix matrix);
QVideoFrameFormat::ColorRange qtColorRangeFor(ColorRange range);
```

In `playback/output/qtpreviewsink.cpp`, add the helpers above `QtPreviewSink::toQVideoFrame` and replace the height>576 branch ([qtpreviewsink.cpp:23](../../../playback/output/qtpreviewsink.cpp)). The keystone migrated the body to read a `MediaVideoFrameView view(frame);`; the only change here is the color-space lines:

```cpp
QVideoFrameFormat::ColorSpace qtColorSpaceFor(ColorMatrix matrix) {
    switch (matrix) {
    case ColorMatrix::Bt601: return QVideoFrameFormat::ColorSpace_BT601;
    case ColorMatrix::Bt2020: return QVideoFrameFormat::ColorSpace_BT2020;
    case ColorMatrix::Bt709:
    default: return QVideoFrameFormat::ColorSpace_BT709;
    }
}

QVideoFrameFormat::ColorRange qtColorRangeFor(ColorRange range) {
    return range == ColorRange::Full ? QVideoFrameFormat::ColorRange_Full
                                     : QVideoFrameFormat::ColorRange_Video;
}
```

Then, in `toQVideoFrame`, replace:

```cpp
    format.setColorSpace(frame.height > 576 ? QVideoFrameFormat::ColorSpace_BT709
                                            : QVideoFrameFormat::ColorSpace_BT601);
    format.setColorRange(QVideoFrameFormat::ColorRange_Video);
```

with (reading the per-handle color metadata the keystone now carries):

```cpp
    const ColorMetadata& color = frame.metadata().color;
    format.setColorSpace(qtColorSpaceFor(color.matrix));
    format.setColorRange(qtColorRangeFor(color.range));
```

> The local `frame` is the `const FrameHandle&` parameter post-keystone; `frame.metadata().color` is the per-handle override. Geometry/plane reads in the body already go through the `MediaVideoFrameView` the keystone introduced, so `frame.height`/`frame.width`/`frame.planeY` no longer appear directly — only the two color lines change.

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build build/c --target tst_qtpreviewsink && ctest --test-dir build/c -R tst_qtpreviewsink --output-on-failure`
Expected: PASS — the new round-trip/mapping slots plus all pre-existing `tst_qtpreviewsink` slots (`deliverMediaFrameUpdatesProviderLatestImage` etc.) green.

- [ ] **Step 5: Run the full unit label + playback e2e to prove the no-op**

Run: `cmake --build build/c && ctest --test-dir build/c -L unit --output-on-failure`
Expected: PASS — identical assertion values across the suite.

Run: `ctest --test-dir build/c -R e2e_play --output-on-failure`
Expected: PASS — golden outputs byte-identical (untagged fixtures resolve to the height default → the same `ColorSpace_BT709/BT601 + ColorRange_Video` the retired heuristic produced). This is the spec §9 "color is a provable no-op" gate. The deliberate Phase-3 re-bake of tagged fixtures (correct color over the height guess) is explicitly **out of scope** for this plan and is not triggered by any change here.

- [ ] **Step 6: Format changed lines and commit**

```bash
CF=/opt/homebrew/opt/llvm/bin/clang-format
python3 /opt/homebrew/opt/llvm/bin/git-clang-format --binary "$CF" --commit origin/main -- '*.cpp' '*.h'
git add playback/output/qtpreviewsink.h playback/output/qtpreviewsink.cpp \
        tests/unit/tst_qtpreviewsink.cpp
git commit -m "feat(color): preview sink reads ColorMetadata, retiring the height>576 guess"
```

---

## Done-when

- `colorvui.{h,cpp}` (Task 1), `colormetadatapolicy.{h,cpp}` (Task 2), `colortags_apple.{h,mm}` (Task 3) ship, each with a green unit test.
- Every decoded `FrameHandle` carries a resolved `ColorMetadata` (Task 4); the Qt preview sink reads it instead of guessing by height (Task 5).
- The full `-L unit` suite and `e2e_play` pass with **identical assertion values and byte-identical golden outputs** — the Phase-1 provable no-op (spec §7, §9).
- The round-trip unit test (`colorMetadataRoundTripsDecodeToSink`) pins that metadata set at the decode edge arrives at the sink.
- A frame *explicitly* tagged differently from its height default (`taggedBt601FrameMapsToBt601`) maps to the tagged value — proving the sink reads metadata, the seam later subprojects (`gpu-compositor`, `gpu-encode`) consume.
- **Out of scope (spec §9):** the Phase-3 deliberate golden re-bake of tagged fixtures (correct BT.601/709 + range replacing the height guess) is a separate, later event tracked separately; no task here performs or enables it.
