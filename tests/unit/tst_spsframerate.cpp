#include <QtTest>

#include "recorder_engine/ingest/spsframerate.h"

#include <cstdint>
#include <vector>

class TestSpsFrameRate : public QObject {
    Q_OBJECT
private slots:
    void rejectsUnsupportedAndMalformedInput();
    void recoversRateFromVuiTimingInfo();
    void rejectsImplausibleRate();
};

namespace {
// Minimal H.264 SPS bit-writer: emits exactly the field sequence
// parseSpsFrameRate walks (Baseline profile => no high-profile/scaling-matrix
// block), ending the VUI at timing_info, then RBSP emulation-prevention-escapes
// the payload and prepends the SPS NAL header. Correct-by-construction inverse of
// the parser, so a positive parse is a real end-to-end check (not a magic blob).
struct BitWriter {
    std::vector<uint8_t> bytes;
    int bitpos = 0;
    void bit(int b) {
        if (bitpos == 0) bytes.push_back(0);
        if (b) bytes.back() |= uint8_t(1 << (7 - bitpos));
        bitpos = (bitpos + 1) & 7;
    }
    void u(uint32_t v, int n) {
        for (int i = n - 1; i >= 0; --i)
            bit((v >> i) & 1u);
    }
    void ue(uint32_t v) { // Exp-Golomb: floor(log2(v+1)) zeros, then (v+1) in that+1 bits.
        const uint32_t c = v + 1;
        int n = 0;
        while (c >> (n + 1))
            ++n;
        for (int i = 0; i < n; ++i)
            bit(0);
        for (int i = n; i >= 0; --i)
            bit((c >> i) & 1u);
    }
};

QByteArray makeSps(uint32_t numUnitsInTick, uint32_t timeScale) {
    BitWriter w;
    w.ue(0);  // seq_parameter_set_id
    w.ue(0);  // log2_max_frame_num_minus4
    w.ue(0);  // pic_order_cnt_type = 0
    w.ue(0);  //   log2_max_pic_order_cnt_lsb_minus4
    w.ue(0);  // max_num_ref_frames
    w.bit(0); // gaps_in_frame_num_value_allowed_flag
    w.ue(0);  // pic_width_in_mbs_minus1
    w.ue(0);  // pic_height_in_map_units_minus1
    w.bit(1); // frame_mbs_only_flag
    w.bit(0); // direct_8x8_inference_flag
    w.bit(0); // frame_cropping_flag
    w.bit(1); // vui_parameters_present_flag
    w.bit(0); // aspect_ratio_info_present_flag
    w.bit(0); // overscan_info_present_flag
    w.bit(0); // video_signal_type_present_flag
    w.bit(0); // chroma_loc_info_present_flag
    w.bit(1); // timing_info_present_flag
    w.u(numUnitsInTick, 32);
    w.u(timeScale, 32);
    w.bit(0); // fixed_frame_rate_flag (parser stops before this; padding only)
    w.bit(1); // rbsp_stop_one_bit

    QByteArray out;
    out.append(char(0x67)); // NAL header: nal_unit_type = 7 (SPS)
    out.append(char(0x42)); // profile_idc = 66 (Baseline: no high-profile block)
    out.append(char(0x00)); // constraint flags + reserved
    out.append(char(0x1e)); // level_idc = 30
    int zeros = 0;
    for (uint8_t b : w.bytes) {
        if (zeros >= 2 && b <= 0x03) { // emulation_prevention_three_byte
            out.append(char(0x03));
            zeros = 0;
        }
        out.append(char(b));
        zeros = (b == 0x00) ? zeros + 1 : 0;
    }
    return out;
}
} // namespace

void TestSpsFrameRate::rejectsUnsupportedAndMalformedInput() {
    const QList<QByteArray> malformed{QByteArray(), QByteArray::fromHex("67"),
                                      QByteArray::fromHex("68000000"),
                                      QByteArray::fromHex("6764001fac")};
    for (const QByteArray& nal : malformed) {
        const SpsFrameRate rate = parseSpsFrameRate(NativeVideoCodec::H264, nal);
        QCOMPARE(rate.num, int32_t(0));
        QCOMPARE(rate.den, int32_t(0));
    }
    // A valid SPS with a NON-H.264 codec tag still yields nothing (HEVC unsupported here).
    const SpsFrameRate hevc = parseSpsFrameRate(NativeVideoCodec::Hevc, makeSps(1, 120));
    QVERIFY(!hevc.valid());
}

void TestSpsFrameRate::recoversRateFromVuiTimingInfo() {
    // fps = time_scale / (2 * num_units_in_tick).
    const SpsFrameRate p60 = parseSpsFrameRate(NativeVideoCodec::H264, makeSps(1, 120));
    QVERIFY(p60.valid());
    QCOMPARE(double(p60.num) / p60.den, 60.0);

    // 59.94 = 120000 / (2 * 1001) -> reduces to 60000/1001.
    const SpsFrameRate p5994 = parseSpsFrameRate(NativeVideoCodec::H264, makeSps(1001, 120000));
    QVERIFY(p5994.valid());
    QCOMPARE(p5994.num, int32_t(60000));
    QCOMPARE(p5994.den, int32_t(1001));

    // 25 = 50 / (2 * 1).
    const SpsFrameRate p25 = parseSpsFrameRate(NativeVideoCodec::H264, makeSps(1, 50));
    QVERIFY(p25.valid());
    QCOMPARE(double(p25.num) / p25.den, 25.0);
}

void TestSpsFrameRate::rejectsImplausibleRate() {
    // 1000 fps (2000 / 2) is above the [12,240] guard -> {0,0}.
    QVERIFY(!parseSpsFrameRate(NativeVideoCodec::H264, makeSps(1, 2000)).valid());
    // 1 fps (2 / 2) is below the guard -> {0,0}.
    QVERIFY(!parseSpsFrameRate(NativeVideoCodec::H264, makeSps(1, 2)).valid());
}

QTEST_GUILESS_MAIN(TestSpsFrameRate)
#include "tst_spsframerate.moc"
