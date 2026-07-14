#include <QtTest>
#include <QFile>

#include "recorder_engine/ingest/h26xseitimecode.h"
#include "recorder_engine/ingest/h26xtimingcontext.h"
#include "recorder_engine/timing/smpte12m.h"

class TestH26xSeiTimecode : public QObject {
    Q_OBJECT

private slots:
    void h264StandardPicTimingIsDecoded();
    void h264PicTimingWithHrdIsDecoded();
    void h264LeadingBcdWithoutTimestampIsIgnored();
    void h264TruncatedClockTimestampIsRejected();
    void h264TimeOffsetIsBoundsChecked();
    void h264PartialTimestampIsNotReturned();
    void h264OutOfRateFrameLabelIsRejected();
    void h264PicTimingWithoutContextIsRejected();
    void hevcTimeCodeDecoded();
    void hevcSuffixSeiDecoded();
    void noSeiReturnsInvalid();
    void truncatedSeiPayloadReturnsInvalid();
    void truncatedPayloadSizeReturnsInvalid();
    void emptyBufferReturnsInvalid();
    void seiWithoutTimecodePayloadTypeReturnsInvalid();
    void unknownCodecReturnsInvalid();
    void hugePayloadSizeVarintDoesNotOverflowOrCrash();
};

namespace {

const char* kStartCode4 = "\x00\x00\x00\x01";

// Build the big-endian SMPTE 12M 32-bit word for a timecode (the on-wire layout
// the extractor reads back out of the recognised SEI payload).
QByteArray packedWordBytes(const Smpte12mTimecode& tc) {
    const uint32_t word = Smpte12m::toPackedWord(tc);
    QByteArray bytes(4, char(0));
    bytes[0] = char((word >> 24) & 0xFF);
    bytes[1] = char((word >> 16) & 0xFF);
    bytes[2] = char((word >> 8) & 0xFF);
    bytes[3] = char(word & 0xFF);
    return bytes;
}

// Encode a SEI payloadType/payloadSize value as the 0xFF-continuation byte run
// used by the SEI RBSP (a sum of 0xFF bytes followed by a final value < 0xFF).
QByteArray seiVarByte(int value) {
    QByteArray out;
    while (value >= 0xFF) {
        out.append(char(0xFF));
        value -= 0xFF;
    }
    out.append(char(value));
    return out;
}

// Assemble a SEI message body: payloadType, payloadSize, then payload bytes.
QByteArray seiMessage(int payloadType, const QByteArray& payload) {
    return seiVarByte(payloadType) + seiVarByte(payload.size()) + payload;
}

// Wrap a SEI RBSP (one or more messages) into an Annex-B SEI NAL with the given
// NAL header bytes, appending the RBSP trailing-bits stop byte (0x80).
QByteArray seiNal(const QByteArray& header, const QByteArray& rbsp) {
    return QByteArray(kStartCode4, 4) + header + rbsp + QByteArray(1, char(0x80));
}

QByteArray h264SeiNal(const QByteArray& rbsp) {
    return seiNal(QByteArray(1, char(0x06)), rbsp); // nal_type 6
}

QByteArray hevcPrefixSeiNal(const QByteArray& rbsp) {
    // nal_type 39 (PREFIX_SEI): byte0 = 39 << 1 = 0x4E, byte1 = 0x01.
    return seiNal(QByteArray::fromHex("4e01"), rbsp);
}

QByteArray hevcSuffixSeiNal(const QByteArray& rbsp) {
    // nal_type 40 (SUFFIX_SEI): byte0 = 40 << 1 = 0x50, byte1 = 0x01.
    return seiNal(QByteArray::fromHex("5001"), rbsp);
}

// A trivial VCL NAL so the buffer looks like a real access unit.
QByteArray h264VclNal() {
    return QByteArray(kStartCode4, 4) + QByteArray::fromHex("658884"); // IDR slice
}

QByteArray hevcVclNal() {
    return QByteArray(kStartCode4, 4) + QByteArray::fromHex("260100"); // IDR_W_RADL slice
}

QByteArray fixture(const char* name) {
    const QString path =
        QFINDTESTDATA(QStringLiteral("../fixtures/timecode/") + QString::fromLatin1(name));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

QByteArray h264SpsFromAnnexB(const QByteArray& annexB) {
    for (int i = 0; i + 5 <= annexB.size(); ++i) {
        int prefix = 0;
        if (annexB.mid(i, 4) == QByteArray::fromHex("00000001")) {
            prefix = 4;
        } else if (annexB.mid(i, 3) == QByteArray::fromHex("000001")) {
            prefix = 3;
        }
        if (prefix == 0 || (uchar(annexB[i + prefix]) & 0x1f) != 7) {
            continue;
        }
        int end = annexB.size();
        for (int j = i + prefix + 1; j + 3 <= annexB.size(); ++j) {
            if (annexB.mid(j, 3) == QByteArray::fromHex("000001") ||
                (j + 4 <= annexB.size() && annexB.mid(j, 4) == QByteArray::fromHex("00000001"))) {
                end = j;
                break;
            }
        }
        return annexB.mid(i + prefix, end - i - prefix);
    }
    return {};
}

struct BitWriter {
    QByteArray bytes;
    int bitPosition = 0;

    void bit(bool value) {
        if (bitPosition == 0) bytes.append(char(0));
        if (value) bytes[bytes.size() - 1] = char(uchar(bytes.back()) | (1u << (7 - bitPosition)));
        bitPosition = (bitPosition + 1) & 7;
    }

    void bits(uint32_t value, int count) {
        for (int i = count - 1; i >= 0; --i)
            bit(((value >> i) & 1u) != 0);
    }
};

QByteArray fullTimestampPayload(int hours, int minutes, int seconds, int frames) {
    BitWriter writer;
    writer.bits(0, 4); // pic_struct: frame
    writer.bit(true);  // clock_timestamp_flag[0]
    writer.bits(0, 2); // ct_type: progressive
    writer.bit(false); // nuit_field_based_flag
    writer.bits(0, 5); // counting_type: no frame count dropping
    writer.bit(true);  // full_timestamp_flag
    writer.bit(false); // discontinuity_flag
    writer.bit(false); // cnt_dropped_flag
    writer.bits(uint32_t(frames), 8);
    writer.bits(uint32_t(seconds), 6);
    writer.bits(uint32_t(minutes), 6);
    writer.bits(uint32_t(hours), 5);
    return writer.bytes;
}

QByteArray partialTimestampPayload() {
    BitWriter writer;
    writer.bits(0, 4); // pic_struct: frame
    writer.bit(true);  // clock_timestamp_flag[0]
    writer.bits(0, 2); // ct_type: progressive
    writer.bit(false); // nuit_field_based_flag
    writer.bits(0, 5); // counting_type
    writer.bit(false); // full_timestamp_flag
    writer.bit(false); // discontinuity_flag
    writer.bit(false); // cnt_dropped_flag
    writer.bits(4, 8); // n_frames
    writer.bit(true);  // seconds_flag
    writer.bits(3, 6); // seconds_value
    writer.bit(false); // minutes_flag: hours are consequently also absent
    return writer.bytes;
}

} // namespace

void TestH26xSeiTimecode::h264StandardPicTimingIsDecoded() {
    const QByteArray annexB = fixture("h264_pic_timing_no_hrd.264");
    QVERIFY(!annexB.isEmpty());
    H26xTimingContext context;
    QVERIFY(context.updateParameterSets(NativeVideoCodec::H264, {}, {h264SpsFromAnnexB(annexB)}));
    const Smpte12mTimecode got = extractH26xSeiTimecode(annexB, NativeVideoCodec::H264, context);
    QVERIFY(got.valid);
    QCOMPARE(got.hours, 1);
    QCOMPARE(got.minutes, 2);
    QCOMPARE(got.seconds, 3);
    QCOMPARE(got.frames, 4);
}

void TestH26xSeiTimecode::h264PicTimingWithHrdIsDecoded() {
    H26xTimingContext context;
    QVERIFY(context.updateParameterSets(NativeVideoCodec::H264, {}, {fixture("h264_sps_hrd.bin")}));
    const Smpte12mTimecode got =
        extractH26xSeiTimecode(fixture("h264_pic_timing_hrd.264"), NativeVideoCodec::H264, context);
    QVERIFY(got.valid);
    QCOMPARE(got.hours, 10);
    QCOMPARE(got.minutes, 11);
    QCOMPARE(got.seconds, 12);
    QCOMPARE(got.frames, 13);
}

void TestH26xSeiTimecode::h264LeadingBcdWithoutTimestampIsIgnored() {
    H26xTimingContext context;
    QVERIFY(context.updateParameterSets(NativeVideoCodec::H264, {}, {fixture("h264_sps_hrd.bin")}));
    const Smpte12mTimecode got = extractH26xSeiTimecode(fixture("h264_bcd_false_positive.264"),
                                                        NativeVideoCodec::H264, context);
    QVERIFY(!got.valid);
}

void TestH26xSeiTimecode::h264TruncatedClockTimestampIsRejected() {
    H26xTimingContext context;
    QVERIFY(context.updateParameterSets(NativeVideoCodec::H264, {}, {fixture("h264_sps_hrd.bin")}));
    QByteArray truncated = fixture("h264_pic_timing_hrd.264");
    truncated.truncate(13); // declared eight-byte pic_timing payload has only six bytes
    const Smpte12mTimecode got = extractH26xSeiTimecode(truncated, NativeVideoCodec::H264, context);
    QVERIFY(!got.valid);
}

void TestH26xSeiTimecode::h264TimeOffsetIsBoundsChecked() {
    H26xTimingContext context;
    QVERIFY(context.updateParameterSets(NativeVideoCodec::H264, {}, {fixture("h264_sps_hrd.bin")}));
    H264TimingSyntax constructedSyntax = *context.h264();
    constructedSyntax.timeOffsetLength = 5;
    QByteArray payload = QByteArray::fromHex("0441840206985a80");
    auto parsed = H26xTimingDetail::parseH264PicTiming(payload, constructedSyntax);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Valid);
    QVERIFY(parsed.timecode.valid);

    constructedSyntax.timeOffsetLength = 24;
    parsed = H26xTimingDetail::parseH264PicTiming(payload, constructedSyntax);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Malformed);
    QVERIFY(!parsed.timecode.valid);
}

void TestH26xSeiTimecode::h264PartialTimestampIsNotReturned() {
    const QByteArray reference = fixture("h264_pic_timing_no_hrd.264");
    H26xTimingContext context;
    QVERIFY(
        context.updateParameterSets(NativeVideoCodec::H264, {}, {h264SpsFromAnnexB(reference)}));
    const QByteArray annexB = h264SeiNal(seiMessage(1, partialTimestampPayload())) + h264VclNal();
    const Smpte12mTimecode parsed = extractH26xSeiTimecode(annexB, NativeVideoCodec::H264, context);
    QVERIFY(!parsed.valid);
}

void TestH26xSeiTimecode::h264OutOfRateFrameLabelIsRejected() {
    const QByteArray reference = fixture("h264_pic_timing_no_hrd.264");
    H26xTimingContext context;
    QVERIFY(
        context.updateParameterSets(NativeVideoCodec::H264, {}, {h264SpsFromAnnexB(reference)}));
    QCOMPARE(context.constantFrameRate(), (FrameRateQ{25, 1}));

    const QByteArray annexB =
        h264SeiNal(seiMessage(1, fullTimestampPayload(1, 2, 3, 25))) + h264VclNal();
    const Smpte12mTimecode parsed = extractH26xSeiTimecode(annexB, NativeVideoCodec::H264, context);
    QVERIFY(!parsed.valid);
}

void TestH26xSeiTimecode::h264PicTimingWithoutContextIsRejected() {
    H26xTimingContext context;
    const Smpte12mTimecode got = extractH26xSeiTimecode(fixture("h264_pic_timing_no_hrd.264"),
                                                        NativeVideoCodec::H264, context);
    QVERIFY(!got.valid);
}

void TestH26xSeiTimecode::hevcTimeCodeDecoded() {
    const Smpte12mTimecode want{1, 2, 3, 4, /*drop*/ false, /*valid*/ true};
    const QByteArray rbsp = seiMessage(/*time_code*/ 136, packedWordBytes(want));
    const QByteArray annexB = hevcPrefixSeiNal(rbsp) + hevcVclNal();

    const Smpte12mTimecode got = extractH26xSeiTimecode(annexB, NativeVideoCodec::Hevc);
    QVERIFY(got.valid);
    QCOMPARE(got.hours, 1);
    QCOMPARE(got.minutes, 2);
    QCOMPARE(got.seconds, 3);
    QCOMPARE(got.frames, 4);
}

void TestH26xSeiTimecode::hevcSuffixSeiDecoded() {
    const Smpte12mTimecode want{23, 59, 58, 24, /*drop*/ false, /*valid*/ true};
    const QByteArray rbsp = seiMessage(136, packedWordBytes(want));
    const QByteArray annexB = hevcVclNal() + hevcSuffixSeiNal(rbsp);

    const Smpte12mTimecode got = extractH26xSeiTimecode(annexB, NativeVideoCodec::Hevc);
    QVERIFY(got.valid);
    QCOMPARE(got.hours, 23);
    QCOMPARE(got.minutes, 59);
    QCOMPARE(got.seconds, 58);
    QCOMPARE(got.frames, 24);
}

void TestH26xSeiTimecode::noSeiReturnsInvalid() {
    const QByteArray annexB = h264VclNal(); // VCL only, no SEI
    const Smpte12mTimecode got = extractH26xSeiTimecode(annexB, NativeVideoCodec::H264);
    QVERIFY(!got.valid);
}

void TestH26xSeiTimecode::truncatedSeiPayloadReturnsInvalid() {
    // payloadType=1, payloadSize=4, but only 2 payload bytes present.
    const QByteArray rbsp = seiVarByte(1) + seiVarByte(4) + QByteArray::fromHex("0a0b");
    const QByteArray annexB = h264SeiNal(rbsp) + h264VclNal();
    const Smpte12mTimecode got = extractH26xSeiTimecode(annexB, NativeVideoCodec::H264);
    QVERIFY(!got.valid); // must not read OOB
}

void TestH26xSeiTimecode::truncatedPayloadSizeReturnsInvalid() {
    // payloadType present, but the buffer ends mid 0xFF continuation run with no
    // terminating size byte.
    const QByteArray rbsp = seiVarByte(1) + QByteArray::fromHex("ffff");
    const QByteArray annexB = h264SeiNal(rbsp) + h264VclNal();
    const Smpte12mTimecode got = extractH26xSeiTimecode(annexB, NativeVideoCodec::H264);
    QVERIFY(!got.valid);
}

void TestH26xSeiTimecode::emptyBufferReturnsInvalid() {
    const Smpte12mTimecode got = extractH26xSeiTimecode(QByteArray(), NativeVideoCodec::H264);
    QVERIFY(!got.valid);
}

void TestH26xSeiTimecode::seiWithoutTimecodePayloadTypeReturnsInvalid() {
    // A SEI message with an unrelated payloadType (e.g. 5 = user_data_unregistered
    // with no recognised TC) yields no timecode.
    const QByteArray rbsp = seiMessage(/*buffering_period*/ 0, QByteArray::fromHex("01020304"));
    const QByteArray annexB = h264SeiNal(rbsp) + h264VclNal();
    const Smpte12mTimecode got = extractH26xSeiTimecode(annexB, NativeVideoCodec::H264);
    QVERIFY(!got.valid);
}

void TestH26xSeiTimecode::unknownCodecReturnsInvalid() {
    const QByteArray rbsp =
        seiMessage(1, packedWordBytes(Smpte12mTimecode{1, 0, 0, 0, false, true}));
    const QByteArray annexB = h264SeiNal(rbsp) + h264VclNal();
    const Smpte12mTimecode got = extractH26xSeiTimecode(annexB, NativeVideoCodec::Unknown);
    QVERIFY(!got.valid);
}

void TestH26xSeiTimecode::hugePayloadSizeVarintDoesNotOverflowOrCrash() {
    // Regression: a non-timecode payloadType (0) followed by a multi-megabyte run
    // of 0xFF size-continuation bytes. A 32-bit accumulator would overflow, the
    // `pos + payloadSize` bound would wrap, `pos` would go negative and the next
    // iteration would index out of bounds (SEGV). Must return invalid, no crash,
    // no OOB (runs clean under ASan/UBSan in the sanitizer CI job).
    QByteArray rbsp;
    rbsp.append(char(0));                           // payloadType = 0 (skips the TC decode branch)
    rbsp.append(QByteArray(8'400'000, char(0xFF))); // enormous size varint run
    rbsp.append(char(0x10));                        // terminating size byte
    rbsp.append(QByteArray::fromHex("0a0b0c0d"));   // a little trailing data
    const QByteArray annexB = h264SeiNal(rbsp) + h264VclNal();
    const Smpte12mTimecode got = extractH26xSeiTimecode(annexB, NativeVideoCodec::H264);
    QVERIFY(!got.valid);
}

QTEST_GUILESS_MAIN(TestH26xSeiTimecode)
#include "tst_h26xseitimecode.moc"
