#include <QtTest>
#include <QFile>

#include "recorder_engine/ingest/h26xtimingcontext.h"
#include "recorder_engine/ingest/spsframerate.h"

#include <cstdint>
#include <vector>

class TestSpsFrameRate : public QObject {
    Q_OBJECT
private slots:
    void rejectsUnsupportedAndMalformedInput();
    void recoversRateFromVuiTimingInfo();
    void requiresFixedFrameRateFlag();
    void skipsHighProfileScalingLists();
    void rejectsImplausibleRate();
    void cachesTimingContextByParameterSetBytes();
    void h264CacheIgnoresVpsBytes();
    void rejectsMalformedEmulationPrevention();
    void rejectsInvalidH264SpsNalHeader();
    void rejectsOutOfRangeCoreSyntax_data();
    void rejectsOutOfRangeCoreSyntax();
    void noVuiStillRequiresCompleteRbsp_data();
    void noVuiStillRequiresCompleteRbsp();
    void rejectsBytesAfterRbspTrailingBits_data();
    void rejectsBytesAfterRbspTrailingBits();
    void reportsMalformedAndUnsupportedContext();
    void multiSpsStatusIsOrderIndependent();
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

QByteArray makeSps(uint32_t numUnitsInTick, uint32_t timeScale, bool fixedFrameRate = true,
                   bool highProfileScalingList = false, uint32_t sequenceParameterSetId = 0,
                   uint32_t log2MaxFrameNumMinus4 = 0, uint32_t log2MaxPicOrderCntLsbMinus4 = 0,
                   bool vuiPresent = true, bool writeTrailingBits = true) {
    BitWriter w;
    w.ue(sequenceParameterSetId);
    if (highProfileScalingList) {
        w.ue(1);  // chroma_format_idc = 4:2:0
        w.ue(0);  // bit_depth_luma_minus8
        w.ue(0);  // bit_depth_chroma_minus8
        w.bit(0); // qpprime_y_zero_transform_bypass_flag
        w.bit(1); // seq_scaling_matrix_present_flag
        w.bit(1); // seq_scaling_list_present_flag[0]
        for (int i = 0; i < 16; ++i)
            w.ue(0); // delta_scale = se(v) 0
        for (int i = 1; i < 8; ++i)
            w.bit(0); // remaining scaling lists use defaults
    }
    w.ue(log2MaxFrameNumMinus4);
    w.ue(0);  // pic_order_cnt_type = 0
    w.ue(log2MaxPicOrderCntLsbMinus4);
    w.ue(0);  // max_num_ref_frames
    w.bit(0); // gaps_in_frame_num_value_allowed_flag
    w.ue(0);  // pic_width_in_mbs_minus1
    w.ue(0);  // pic_height_in_map_units_minus1
    w.bit(1); // frame_mbs_only_flag
    w.bit(0); // direct_8x8_inference_flag
    w.bit(0); // frame_cropping_flag
    w.bit(vuiPresent);
    if (vuiPresent) {
        w.bit(0); // aspect_ratio_info_present_flag
        w.bit(0); // overscan_info_present_flag
        w.bit(0); // video_signal_type_present_flag
        w.bit(0); // chroma_loc_info_present_flag
        w.bit(1); // timing_info_present_flag
        w.u(numUnitsInTick, 32);
        w.u(timeScale, 32);
        w.bit(fixedFrameRate ? 1 : 0);
        w.bit(0); // nal_hrd_parameters_present_flag
        w.bit(0); // vcl_hrd_parameters_present_flag
        w.bit(1); // pic_struct_present_flag
        w.bit(0); // bitstream_restriction_flag
    }
    if (writeTrailingBits) w.bit(1); // rbsp_stop_one_bit

    QByteArray out;
    out.append(char(0x67)); // NAL header: nal_unit_type = 7 (SPS)
    out.append(char(highProfileScalingList ? 0x64 : 0x42));
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

QByteArray fixture(const char* name) {
    const QString path =
        QFINDTESTDATA(QStringLiteral("../fixtures/timecode/") + QString::fromLatin1(name));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
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

void TestSpsFrameRate::requiresFixedFrameRateFlag() {
    QVERIFY(!parseSpsFrameRate(NativeVideoCodec::H264, makeSps(1001, 60000, false)).valid());

    H26xTimingContext context;
    QVERIFY(context.updateParameterSets(NativeVideoCodec::H264, {}, {makeSps(1001, 60000, false)}));
    QVERIFY(context.h264() != nullptr);
    QCOMPARE(context.h264()->status, H26xTimingSyntaxStatus::Valid);
    QCOMPARE(context.h264()->frameRate, (FrameRateQ{30000, 1001}));
    QCOMPARE(context.h264()->numUnitsInTick, uint32_t(1001));
    QCOMPARE(context.h264()->timeScale, uint32_t(60000));
    QVERIFY(!context.fixedFrameRate());
    QVERIFY(!context.constantFrameRate().valid());
}

void TestSpsFrameRate::skipsHighProfileScalingLists() {
    const SpsFrameRate rate =
        parseSpsFrameRate(NativeVideoCodec::H264, makeSps(1001, 60000, true, true));
    QVERIFY(rate.valid());
    QCOMPARE(rate.num, int32_t(30000));
    QCOMPARE(rate.den, int32_t(1001));
}

void TestSpsFrameRate::rejectsImplausibleRate() {
    // 1000 fps (2000 / 2) is above the [12,240] guard -> {0,0}.
    QVERIFY(!parseSpsFrameRate(NativeVideoCodec::H264, makeSps(1, 2000)).valid());
    // 1 fps (2 / 2) is below the guard -> {0,0}.
    QVERIFY(!parseSpsFrameRate(NativeVideoCodec::H264, makeSps(1, 2)).valid());
}

void TestSpsFrameRate::cachesTimingContextByParameterSetBytes() {
    H26xTimingContext context;
    const QList<QByteArray> sps{fixture("h264_sps_hrd.bin")};
    QVERIFY(!sps.first().isEmpty());
    QVERIFY(context.updateParameterSets(NativeVideoCodec::H264, {}, sps));
    QCOMPARE(context.codec(), NativeVideoCodec::H264);
    QCOMPARE(context.generation(), uint64_t(1));
    QCOMPARE(context.constantFrameRate(), (FrameRateQ{30000, 1001}));
    QVERIFY(context.fixedFrameRate());
    QVERIFY(context.h264() != nullptr);
    QCOMPARE(context.h264()->status, H26xTimingSyntaxStatus::Valid);
    QCOMPARE(context.h264()->cpbRemovalDelayLength, uint8_t(10));
    QCOMPARE(context.h264()->dpbOutputDelayLength, uint8_t(7));
    QCOMPARE(context.h264()->timeOffsetLength, uint8_t(0));
    QVERIFY(context.h264()->picStructPresent);

    for (int i = 0; i < 1000; ++i)
        QVERIFY(context.updateParameterSets(NativeVideoCodec::H264, {}, sps));
    QCOMPARE(context.generation(), uint64_t(1));

    const QList<QByteArray> changed{makeSps(1, 50)};
    QVERIFY(context.updateParameterSets(NativeVideoCodec::H264, {}, changed));
    QCOMPARE(context.generation(), uint64_t(2));
    QCOMPARE(context.constantFrameRate(), (FrameRateQ{25, 1}));
    QCOMPARE(context.h264()->cpbRemovalDelayLength, uint8_t(0));
    QCOMPARE(context.h264()->dpbOutputDelayLength, uint8_t(0));
}

void TestSpsFrameRate::h264CacheIgnoresVpsBytes() {
    H26xTimingContext context;
    const QList<QByteArray> sps{fixture("h264_sps_hrd.bin")};
    QVERIFY(context.updateParameterSets(NativeVideoCodec::H264, {QByteArrayLiteral("first")}, sps));
    QCOMPARE(context.generation(), uint64_t(1));

    QVERIFY(
        context.updateParameterSets(NativeVideoCodec::H264, {QByteArrayLiteral("changed")}, sps));
    QCOMPARE(context.generation(), uint64_t(1));
    QCOMPARE(context.constantFrameRate(), (FrameRateQ{30000, 1001}));
}

void TestSpsFrameRate::rejectsMalformedEmulationPrevention() {
    H26xTimingContext context;

    QByteArray invalidFollowingByte = makeSps(8, 192);
    const int insertionPoint = invalidFollowingByte.indexOf(QByteArray::fromHex("000004"));
    QVERIFY(insertionPoint >= 0);
    invalidFollowingByte.insert(insertionPoint + 2, char(0x03));
    QVERIFY(!context.updateParameterSets(NativeVideoCodec::H264, {}, {invalidFollowingByte}));
    QCOMPARE(context.h264()->status, H26xTimingSyntaxStatus::Malformed);

    const QByteArray terminalEscape = QByteArray::fromHex("6742001efb9080000003");
    QVERIFY(!context.updateParameterSets(NativeVideoCodec::H264, {}, {terminalEscape}));
    QCOMPARE(context.h264()->status, H26xTimingSyntaxStatus::Malformed);

    for (const QByteArray& forbidden :
         {QByteArray::fromHex("000000"), QByteArray::fromHex("000001"),
          QByteArray::fromHex("000002")}) {
        const QByteArray rawSps = makeSps(8, 192) + forbidden;
        QVERIFY(!context.updateParameterSets(NativeVideoCodec::H264, {}, {rawSps}));
        QCOMPARE(context.h264()->status, H26xTimingSyntaxStatus::Malformed);
    }
}

void TestSpsFrameRate::rejectsInvalidH264SpsNalHeader() {
    QByteArray sps = makeSps(1, 50);
    sps[0] = char(0x07); // nal_ref_idc equal to zero is forbidden for SPS NAL units

    QVERIFY(!parseSpsFrameRate(NativeVideoCodec::H264, sps).valid());
    H26xTimingContext context;
    QVERIFY(!context.updateParameterSets(NativeVideoCodec::H264, {}, {sps}));
    QVERIFY(context.h264() != nullptr);
    QCOMPARE(context.h264()->status, H26xTimingSyntaxStatus::Malformed);
    QVERIFY(!context.constantFrameRate().valid());
}

void TestSpsFrameRate::rejectsOutOfRangeCoreSyntax_data() {
    QTest::addColumn<int>("sequenceParameterSetId");
    QTest::addColumn<int>("log2MaxFrameNumMinus4");
    QTest::addColumn<int>("log2MaxPicOrderCntLsbMinus4");

    QTest::newRow("seq_parameter_set_id 32") << 32 << 0 << 0;
    QTest::newRow("log2_max_frame_num_minus4 13") << 0 << 13 << 0;
    QTest::newRow("log2_max_pic_order_cnt_lsb_minus4 13") << 0 << 0 << 13;
}

void TestSpsFrameRate::rejectsOutOfRangeCoreSyntax() {
    QFETCH(int, sequenceParameterSetId);
    QFETCH(int, log2MaxFrameNumMinus4);
    QFETCH(int, log2MaxPicOrderCntLsbMinus4);

    const QByteArray sps =
        makeSps(1, 50, true, false, uint32_t(sequenceParameterSetId),
                uint32_t(log2MaxFrameNumMinus4), uint32_t(log2MaxPicOrderCntLsbMinus4));
    H26xTimingContext context;
    QVERIFY(!context.updateParameterSets(NativeVideoCodec::H264, {}, {sps}));
    QVERIFY(context.h264() != nullptr);
    QCOMPARE(context.h264()->status, H26xTimingSyntaxStatus::Malformed);
    QVERIFY(!context.constantFrameRate().valid());
}

void TestSpsFrameRate::noVuiStillRequiresCompleteRbsp_data() {
    QTest::addColumn<bool>("writeTrailingBits");
    QTest::addColumn<QByteArray>("tail");
    QTest::addColumn<bool>("expectedValid");

    QTest::newRow("exact no-VUI RBSP") << true << QByteArray() << true;
    QTest::newRow("missing rbsp_stop_one_bit") << false << QByteArray() << false;
    QTest::newRow("appended nonzero byte") << true << QByteArray::fromHex("55") << false;
    QTest::newRow("appended zero byte") << true << QByteArray::fromHex("00") << false;
}

void TestSpsFrameRate::noVuiStillRequiresCompleteRbsp() {
    QFETCH(bool, writeTrailingBits);
    QFETCH(QByteArray, tail);
    QFETCH(bool, expectedValid);

    const QByteArray sps = makeSps(1, 50, true, false, 0, 0, 0, false, writeTrailingBits) + tail;
    H26xTimingContext context;
    QCOMPARE(context.updateParameterSets(NativeVideoCodec::H264, {}, {sps}), expectedValid);
    QVERIFY(context.h264() != nullptr);
    QCOMPARE(context.h264()->status,
             expectedValid ? H26xTimingSyntaxStatus::Valid : H26xTimingSyntaxStatus::Malformed);
    QVERIFY(!context.constantFrameRate().valid());
}

void TestSpsFrameRate::rejectsBytesAfterRbspTrailingBits_data() {
    QTest::addColumn<QByteArray>("prefix");
    QTest::addColumn<int>("extraZeroBytes");
    QTest::addColumn<bool>("expectedValid");

    for (const auto& prefix :
         {QByteArray(), QByteArray::fromHex("000001"), QByteArray::fromHex("00000001")}) {
        const QString prefixName = prefix.isEmpty()
                                       ? QStringLiteral("raw")
                                       : QStringLiteral("prefix-%1-byte").arg(prefix.size());
        QTest::newRow(qPrintable(prefixName + QStringLiteral(" exact RBSP")))
            << prefix << 0 << true;
        for (const int zeroCount : {1, 2}) {
            QTest::newRow(
                qPrintable(prefixName + QStringLiteral(" plus %1 zero bytes").arg(zeroCount)))
                << prefix << zeroCount << false;
        }
    }
}

void TestSpsFrameRate::rejectsBytesAfterRbspTrailingBits() {
    QFETCH(QByteArray, prefix);
    QFETCH(int, extraZeroBytes);
    QFETCH(bool, expectedValid);

    const QByteArray parameterSet = prefix + makeSps(1, 50) + QByteArray(extraZeroBytes, char(0));
    H26xTimingContext context;
    QCOMPARE(context.updateParameterSets(NativeVideoCodec::H264, {}, {parameterSet}),
             expectedValid);
    QVERIFY(context.h264() != nullptr);
    QCOMPARE(context.h264()->status,
             expectedValid ? H26xTimingSyntaxStatus::Valid : H26xTimingSyntaxStatus::Malformed);
}

void TestSpsFrameRate::reportsMalformedAndUnsupportedContext() {
    H26xTimingContext context;
    const QList<QByteArray> malformed{QByteArray::fromHex("6764001fac")};
    QVERIFY(!context.updateParameterSets(NativeVideoCodec::H264, {}, malformed));
    QCOMPARE(context.generation(), uint64_t(1));
    QVERIFY(context.h264() != nullptr);
    QCOMPARE(context.h264()->status, H26xTimingSyntaxStatus::Malformed);
    QVERIFY(!context.constantFrameRate().valid());
    QVERIFY(!context.fixedFrameRate());

    QVERIFY(!context.updateParameterSets(NativeVideoCodec::H264, {}, malformed));
    QCOMPARE(context.generation(), uint64_t(1));

    const QList<QByteArray> mixed{makeSps(1, 50), malformed.first()};
    QVERIFY(!context.updateParameterSets(NativeVideoCodec::H264, {}, mixed));
    QCOMPARE(context.h264()->status, H26xTimingSyntaxStatus::Malformed);

    QByteArray unsupported = makeSps(1, 50);
    unsupported[1] = char(1); // unknown profile_idc: syntax shape cannot be inferred safely
    QVERIFY(!context.updateParameterSets(NativeVideoCodec::H264, {}, {unsupported}));
    QCOMPARE(context.generation(), uint64_t(3));
    QCOMPARE(context.h264()->status, H26xTimingSyntaxStatus::Unsupported);

    QVERIFY(!context.updateParameterSets(NativeVideoCodec::Hevc, {QByteArray::fromHex("40")},
                                         {QByteArray::fromHex("42")}));
    QCOMPARE(context.generation(), uint64_t(4));
    QVERIFY(context.h264() == nullptr);
    QVERIFY(context.hevc() != nullptr);
    QCOMPARE(context.hevc()->status, H26xTimingSyntaxStatus::Unsupported);
}

void TestSpsFrameRate::multiSpsStatusIsOrderIndependent() {
    const QByteArray valid25 = makeSps(1, 50);
    const QByteArray equivalent25 = makeSps(2, 100);
    const QByteArray malformed = QByteArray::fromHex("6764001fac");
    QByteArray unsupported = makeSps(1, 50);
    unsupported[1] = char(1);

    for (const QList<QByteArray>& entries :
         {QList<QByteArray>{unsupported, malformed}, QList<QByteArray>{malformed, unsupported}}) {
        H26xTimingContext context;
        QVERIFY(!context.updateParameterSets(NativeVideoCodec::H264, {}, entries));
        QCOMPARE(context.h264()->status, H26xTimingSyntaxStatus::Malformed);
    }

    for (const QList<QByteArray>& entries :
         {QList<QByteArray>{valid25, equivalent25}, QList<QByteArray>{equivalent25, valid25}}) {
        H26xTimingContext context;
        QVERIFY(context.updateParameterSets(NativeVideoCodec::H264, {}, entries));
        QCOMPARE(context.h264()->status, H26xTimingSyntaxStatus::Valid);
        QCOMPARE(context.constantFrameRate(), (FrameRateQ{25, 1}));
    }

    for (const QList<QByteArray>& entries :
         {QList<QByteArray>{valid25, unsupported}, QList<QByteArray>{unsupported, valid25}}) {
        H26xTimingContext context;
        QVERIFY(!context.updateParameterSets(NativeVideoCodec::H264, {}, entries));
        QCOMPARE(context.h264()->status, H26xTimingSyntaxStatus::Unsupported);
    }
}

QTEST_GUILESS_MAIN(TestSpsFrameRate)
#include "tst_spsframerate.moc"
