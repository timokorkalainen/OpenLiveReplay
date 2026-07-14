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
    void h264ConsumesEveryClockTimestamp();
    void h264RejectsInvalidTimestampLabels();
    void h264InterpretsCountingTypeAndDroppedFlag();
    void h264RejectsInvalidPayloadAlignment();
    void h264RejectsMalformedEmulationPrevention();
    void h264MalformedContextIsNotUnsupported();
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

QByteArray escapeRbsp(const QByteArray& rbsp) {
    QByteArray escaped;
    int zeroCount = 0;
    for (char byte : rbsp) {
        const uchar value = uchar(byte);
        if (zeroCount >= 2 && value <= 0x03) {
            escaped.append(char(0x03));
            zeroCount = 0;
        }
        escaped.append(byte);
        zeroCount = value == 0 ? zeroCount + 1 : 0;
    }
    return escaped;
}

// Wrap a SEI RBSP (one or more messages) into an Annex-B SEI NAL with the given
// NAL header bytes, appending the RBSP trailing-bits stop byte (0x80).
QByteArray seiNal(const QByteArray& header, const QByteArray& rbsp) {
    return QByteArray(kStartCode4, 4) + header + escapeRbsp(rbsp + QByteArray(1, char(0x80)));
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

    void payloadTrailingBits(bool marker = true, bool nonzeroPadding = false) {
        bit(marker);
        bool firstPaddingBit = true;
        while (bitPosition != 0) {
            bit(nonzeroPadding && firstPaddingBit);
            firstPaddingBit = false;
        }
    }
};

void writeFullTimestamp(BitWriter& writer, int hours, int minutes, int seconds, int frames,
                        int countingType = 0, bool countDropped = false) {
    writer.bit(true);  // clock_timestamp_flag[0]
    writer.bits(0, 2); // ct_type: progressive
    writer.bit(false); // nuit_field_based_flag
    writer.bits(uint32_t(countingType), 5);
    writer.bit(true);  // full_timestamp_flag
    writer.bit(false); // discontinuity_flag
    writer.bit(countDropped);
    writer.bits(uint32_t(frames), 8);
    writer.bits(uint32_t(seconds), 6);
    writer.bits(uint32_t(minutes), 6);
    writer.bits(uint32_t(hours), 5);
}

QByteArray fullTimestampPayload(int hours, int minutes, int seconds, int frames,
                                int countingType = 0, bool countDropped = false) {
    BitWriter writer;
    writer.bits(0, 4); // pic_struct: frame
    writeFullTimestamp(writer, hours, minutes, seconds, frames, countingType, countDropped);
    writer.payloadTrailingBits();
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
    writer.payloadTrailingBits();
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
    QByteArray payload = QByteArray::fromHex("0441840206985a81");
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

void TestH26xSeiTimecode::h264ConsumesEveryClockTimestamp() {
    H264TimingSyntax syntax;
    syntax.status = H26xTimingSyntaxStatus::Valid;
    syntax.frameRate = {25, 1};
    syntax.fixedFrameRate = true;
    syntax.picStructPresent = true;

    BitWriter truncatedLaterClock;
    truncatedLaterClock.bits(3, 4); // pic_struct: top field, bottom field (two clocks)
    writeFullTimestamp(truncatedLaterClock, 1, 2, 3, 4);
    truncatedLaterClock.bit(true);  // clock_timestamp_flag[1]
    truncatedLaterClock.bits(0, 2); // truncated ct_type only
    auto parsed = H26xTimingDetail::parseH264PicTiming(truncatedLaterClock.bytes, syntax);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Malformed);
    QVERIFY(!parsed.timecode.valid);

    BitWriter malformedLaterClock;
    malformedLaterClock.bits(3, 4);
    writeFullTimestamp(malformedLaterClock, 1, 2, 3, 4);
    writeFullTimestamp(malformedLaterClock, 24, 0, 0, 0);
    malformedLaterClock.payloadTrailingBits();
    parsed = H26xTimingDetail::parseH264PicTiming(malformedLaterClock.bytes, syntax);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Malformed);
    QVERIFY(!parsed.timecode.valid);

    BitWriter malformedThirdClock;
    malformedThirdClock.bits(5, 4); // pic_struct: three clock_timestamp_flag entries
    writeFullTimestamp(malformedThirdClock, 1, 2, 3, 4);
    malformedThirdClock.bit(false);
    malformedThirdClock.bit(true);
    malformedThirdClock.bits(0, 1); // truncated clock_timestamp[2]
    parsed = H26xTimingDetail::parseH264PicTiming(malformedThirdClock.bytes, syntax);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Malformed);
    QVERIFY(!parsed.timecode.valid);
}

void TestH26xSeiTimecode::h264RejectsInvalidTimestampLabels() {
    H264TimingSyntax syntax;
    syntax.status = H26xTimingSyntaxStatus::Valid;
    syntax.frameRate = {25, 1};
    syntax.fixedFrameRate = true;
    syntax.picStructPresent = true;

    const struct {
        int hours;
        int minutes;
        int seconds;
        int frames;
        int countingType;
    } cases[] = {
        {24, 0, 0, 0, 0}, {0, 60, 0, 0, 0}, {0, 0, 60, 0, 0}, {0, 0, 0, 25, 0}, {0, 0, 0, 0, 7}};
    for (const auto& test : cases) {
        const auto parsed = H26xTimingDetail::parseH264PicTiming(
            fullTimestampPayload(test.hours, test.minutes, test.seconds, test.frames,
                                 test.countingType),
            syntax);
        QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Malformed);
        QVERIFY(!parsed.timecode.valid);
    }
}

void TestH26xSeiTimecode::h264InterpretsCountingTypeAndDroppedFlag() {
    H264TimingSyntax syntax;
    syntax.status = H26xTimingSyntaxStatus::Valid;
    syntax.frameRate = {30000, 1001};
    syntax.fixedFrameRate = true;
    syntax.picStructPresent = true;

    struct Case {
        int countingType;
        bool countDropped;
        int minutes;
        int seconds;
        int frames;
        H26xTimingDetail::TimecodeParseStatus status;
        bool dropFrame;
    };
    const Case cases[] = {
        {0, false, 1, 0, 0, H26xTimingDetail::TimecodeParseStatus::Valid, false},
        {0, true, 1, 0, 0, H26xTimingDetail::TimecodeParseStatus::Malformed, false},
        {1, false, 1, 0, 0, H26xTimingDetail::TimecodeParseStatus::Valid, false},
        {1, true, 1, 0, 0, H26xTimingDetail::TimecodeParseStatus::Malformed, false},
        {2, false, 1, 0, 0, H26xTimingDetail::TimecodeParseStatus::Valid, false},
        {2, true, 1, 0, 1, H26xTimingDetail::TimecodeParseStatus::Unsupported, false},
        {2, true, 1, 0, 2, H26xTimingDetail::TimecodeParseStatus::Malformed, false},
        {3, false, 1, 0, 0, H26xTimingDetail::TimecodeParseStatus::Valid, false},
        {3, true, 1, 0, 0, H26xTimingDetail::TimecodeParseStatus::Unsupported, false},
        {3, true, 1, 0, 1, H26xTimingDetail::TimecodeParseStatus::Malformed, false},
        {4, false, 1, 0, 0, H26xTimingDetail::TimecodeParseStatus::Valid, false},
        {4, true, 1, 0, 2, H26xTimingDetail::TimecodeParseStatus::Valid, true},
        {4, true, 0, 0, 2, H26xTimingDetail::TimecodeParseStatus::Malformed, false},
        {4, true, 1, 1, 2, H26xTimingDetail::TimecodeParseStatus::Malformed, false},
        {4, true, 1, 0, 3, H26xTimingDetail::TimecodeParseStatus::Malformed, false},
        {5, false, 1, 0, 0, H26xTimingDetail::TimecodeParseStatus::Valid, false},
        {5, true, 1, 0, 2, H26xTimingDetail::TimecodeParseStatus::Unsupported, false},
        {6, false, 1, 0, 0, H26xTimingDetail::TimecodeParseStatus::Valid, false},
        {6, true, 1, 0, 2, H26xTimingDetail::TimecodeParseStatus::Unsupported, false},
        {7, false, 1, 0, 0, H26xTimingDetail::TimecodeParseStatus::Malformed, false},
        {31, true, 1, 0, 0, H26xTimingDetail::TimecodeParseStatus::Malformed, false},
    };

    for (const Case& test : cases) {
        const auto parsed = H26xTimingDetail::parseH264PicTiming(
            fullTimestampPayload(1, test.minutes, test.seconds, test.frames, test.countingType,
                                 test.countDropped),
            syntax);
        QCOMPARE(parsed.status, test.status);
        QCOMPARE(parsed.timecode.valid,
                 test.status == H26xTimingDetail::TimecodeParseStatus::Valid);
        if (parsed.timecode.valid) QCOMPARE(parsed.timecode.dropFrame, test.dropFrame);
    }

    BitWriter inheritedDropUnits;
    inheritedDropUnits.bits(0, 4);
    inheritedDropUnits.bit(true);  // clock_timestamp_flag
    inheritedDropUnits.bits(0, 2); // ct_type
    inheritedDropUnits.bit(false); // nuit_field_based_flag
    inheritedDropUnits.bits(4, 5); // counting_type: NTSC two-lowest-count method
    inheritedDropUnits.bit(false); // full_timestamp_flag
    inheritedDropUnits.bit(false); // discontinuity_flag
    inheritedDropUnits.bit(true);  // cnt_dropped_flag
    inheritedDropUnits.bits(2, 8); // n_frames
    inheritedDropUnits.bit(false); // seconds_flag: inherit prior seconds/minutes
    inheritedDropUnits.payloadTrailingBits();
    const auto inherited = H26xTimingDetail::parseH264PicTiming(inheritedDropUnits.bytes, syntax);
    QCOMPARE(inherited.status, H26xTimingDetail::TimecodeParseStatus::Unsupported);
    QVERIFY(!inherited.timecode.valid);
}

void TestH26xSeiTimecode::h264RejectsInvalidPayloadAlignment() {
    H264TimingSyntax syntax;
    syntax.status = H26xTimingSyntaxStatus::Valid;
    syntax.frameRate = {25, 1};
    syntax.fixedFrameRate = true;
    syntax.picStructPresent = true;

    BitWriter missingMarker;
    missingMarker.bits(0, 4);
    writeFullTimestamp(missingMarker, 1, 2, 3, 4);
    missingMarker.payloadTrailingBits(false);
    auto parsed = H26xTimingDetail::parseH264PicTiming(missingMarker.bytes, syntax);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Malformed);

    BitWriter nonzeroPadding;
    nonzeroPadding.bits(0, 4);
    writeFullTimestamp(nonzeroPadding, 1, 2, 3, 4);
    nonzeroPadding.payloadTrailingBits(true, true);
    parsed = H26xTimingDetail::parseH264PicTiming(nonzeroPadding.bytes, syntax);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Malformed);

    H264TimingSyntax noPicStruct = syntax;
    noPicStruct.picStructPresent = false;
    parsed = H26xTimingDetail::parseH264PicTiming({}, noPicStruct);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::NoTimestamp);
    parsed = H26xTimingDetail::parseH264PicTiming(QByteArray::fromHex("00"), noPicStruct);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Malformed);
    parsed = H26xTimingDetail::parseH264PicTiming(QByteArray::fromHex("80"), noPicStruct);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Malformed);

    H264TimingSyntax byteAligned = syntax;
    byteAligned.timeOffsetLength = 7;
    BitWriter exactByteAligned;
    exactByteAligned.bits(0, 4);
    writeFullTimestamp(exactByteAligned, 1, 2, 3, 4);
    exactByteAligned.bits(0, 7); // time_offset: syntax ends on the byte boundary
    QCOMPARE(exactByteAligned.bitPosition, 0);
    parsed = H26xTimingDetail::parseH264PicTiming(exactByteAligned.bytes, byteAligned);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Valid);
    QVERIFY(parsed.timecode.valid);

    H264TimingSyntax byteAlignedNoTimestamp = syntax;
    byteAlignedNoTimestamp.picStructPresent = false;
    byteAlignedNoTimestamp.cpbDpbDelaysPresent = true;
    byteAlignedNoTimestamp.cpbRemovalDelayLength = 4;
    byteAlignedNoTimestamp.dpbOutputDelayLength = 4;
    parsed =
        H26xTimingDetail::parseH264PicTiming(QByteArray::fromHex("00"), byteAlignedNoTimestamp);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::NoTimestamp);

    BitWriter reservedPicStruct;
    reservedPicStruct.bits(9, 4);
    reservedPicStruct.payloadTrailingBits();
    parsed = H26xTimingDetail::parseH264PicTiming(reservedPicStruct.bytes, syntax);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Malformed);
}

void TestH26xSeiTimecode::h264RejectsMalformedEmulationPrevention() {
    const QByteArray reference = fixture("h264_pic_timing_no_hrd.264");
    H26xTimingContext context;
    QVERIFY(
        context.updateParameterSets(NativeVideoCodec::H264, {}, {h264SpsFromAnnexB(reference)}));

    const QByteArray validMessage = seiMessage(1, fullTimestampPayload(1, 2, 3, 4));
    const QByteArray invalidFollowingByte = QByteArray(kStartCode4, 4) + QByteArray(1, char(0x06)) +
                                            QByteArray::fromHex("000300000304") + validMessage +
                                            QByteArray(1, char(0x80));
    QVERIFY(!extractH26xSeiTimecode(invalidFollowingByte, NativeVideoCodec::H264, context).valid);

    const QByteArray terminalEscape = QByteArray(kStartCode4, 4) + QByteArray(1, char(0x06)) +
                                      validMessage + QByteArray::fromHex("000003");
    QVERIFY(!extractH26xSeiTimecode(terminalEscape, NativeVideoCodec::H264, context).valid);

    for (const QByteArray& forbidden :
         {QByteArray::fromHex("000000"), QByteArray::fromHex("000002")}) {
        const QByteArray rawNal = QByteArray(kStartCode4, 4) + QByteArray(1, char(0x06)) +
                                  forbidden + validMessage + QByteArray(1, char(0x80));
        QVERIFY(!extractH26xSeiTimecode(rawNal, NativeVideoCodec::H264, context).valid);
    }

    const QByteArray escapedPayload = seiMessage(5, QByteArray::fromHex("000002")) + validMessage;
    const auto escapedResult =
        extractH26xSeiTimecode(h264SeiNal(escapedPayload), NativeVideoCodec::H264, context);
    QVERIFY(escapedResult.valid);
    QCOMPARE(escapedResult.frames, 4);

    const QByteArray separatedNals = QByteArray(kStartCode4, 4) + QByteArray::fromHex("0605") +
                                     QByteArray::fromHex("000001") + QByteArray(1, char(0x06)) +
                                     escapeRbsp(validMessage + QByteArray(1, char(0x80)));
    const auto splitResult = extractH26xSeiTimecode(separatedNals, NativeVideoCodec::H264, context);
    QVERIFY(splitResult.valid);
    QCOMPARE(splitResult.frames, 4);
}

void TestH26xSeiTimecode::h264MalformedContextIsNotUnsupported() {
    H264TimingSyntax syntax;
    syntax.status = H26xTimingSyntaxStatus::Malformed;
    const auto parsed = H26xTimingDetail::parseH264PicTiming({}, syntax);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Malformed);
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
