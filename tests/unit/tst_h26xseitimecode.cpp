#include <QtTest>
#include <QFile>

#include <limits>

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
    void h264ClockTimestampsAreNondecreasing_data();
    void h264ClockTimestampsAreNondecreasing();
    void h264ConsecutiveFieldTimestampsRespectCtType_data();
    void h264ConsecutiveFieldTimestampsRespectCtType();
    void h264CountingSchemesApplyOffsetsBeforeClassification_data();
    void h264CountingSchemesApplyOffsetsBeforeClassification();
    void h264NonCfrTimingIsValidatedBeforeUnsupported();
    void h264SeiMessagesAreFullyValidated();
    void h264SeiMessagesAggregateTypedResults();
    void h264LaterIncompleteClockInheritsUnits_data();
    void h264LaterIncompleteClockInheritsUnits();
    void h264RejectsInvalidTimestampLabels();
    void h264InterpretsCountingTypeAndDroppedFlag();
    void h264ValidatesPartialTimestampFields_data();
    void h264ValidatesPartialTimestampFields();
    void h264RejectsInvalidPayloadAlignment();
    void h264RejectsMalformedEmulationPrevention();
    void h264RejectsInvalidSeiNalHeaders_data();
    void h264RejectsInvalidSeiNalHeaders();
    void h264AnnexBTrailingZeroBytesAreNotNalPayload();
    void rawEbspTrailingZeroBytesRemainStrict();
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

QByteArray rawH264SeiNal(const QByteArray& rbsp) {
    return QByteArray(kStartCode4, 4) + QByteArray(1, char(0x06)) + escapeRbsp(rbsp);
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

    void signedBits(int32_t value, int count) {
        const uint32_t mask =
            count == 32 ? std::numeric_limits<uint32_t>::max() : (uint32_t(1) << count) - 1u;
        bits(uint32_t(value) & mask, count);
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
                        int countingType = 0, bool countDropped = false, bool nuitFieldBased = true,
                        int32_t timeOffset = 0, int timeOffsetLength = 0,
                        bool discontinuity = false, int ctType = 0) {
    writer.bit(true);  // clock_timestamp_flag[0]
    writer.bits(uint32_t(ctType), 2);
    writer.bit(nuitFieldBased);
    writer.bits(uint32_t(countingType), 5);
    writer.bit(true);  // full_timestamp_flag
    writer.bit(discontinuity);
    writer.bit(countDropped);
    writer.bits(uint32_t(frames), 8);
    writer.bits(uint32_t(seconds), 6);
    writer.bits(uint32_t(minutes), 6);
    writer.bits(uint32_t(hours), 5);
    writer.signedBits(timeOffset, timeOffsetLength);
}

QByteArray fullTimestampPayload(int hours, int minutes, int seconds, int frames,
                                int countingType = 0, bool countDropped = false,
                                bool nuitFieldBased = true) {
    BitWriter writer;
    writer.bits(0, 4); // pic_struct: frame
    writeFullTimestamp(writer, hours, minutes, seconds, frames, countingType, countDropped,
                       nuitFieldBased);
    if (writer.bitPosition != 0) writer.payloadTrailingBits();
    return writer.bytes;
}

QByteArray partialTimestampPayload(int frames = 4, int seconds = 3, int minutes = -1,
                                   int hours = -1) {
    BitWriter writer;
    writer.bits(0, 4); // pic_struct: frame
    writer.bit(true);  // clock_timestamp_flag[0]
    writer.bits(0, 2); // ct_type: progressive
    writer.bit(true);  // nuit_field_based_flag: n_frames has a frame-rate unit
    writer.bits(0, 5); // counting_type
    writer.bit(false); // full_timestamp_flag
    writer.bit(false); // discontinuity_flag
    writer.bit(false); // cnt_dropped_flag
    writer.bits(uint32_t(frames), 8);
    writer.bit(seconds >= 0);
    if (seconds >= 0) {
        writer.bits(uint32_t(seconds), 6);
        writer.bit(minutes >= 0);
        if (minutes >= 0) {
            writer.bits(uint32_t(minutes), 6);
            writer.bit(hours >= 0);
            if (hours >= 0) writer.bits(uint32_t(hours), 5);
        }
    }
    if (writer.bitPosition != 0) writer.payloadTrailingBits();
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
    BitWriter payloadWriter;
    payloadWriter.bits(0, constructedSyntax.cpbRemovalDelayLength);
    payloadWriter.bits(0, constructedSyntax.dpbOutputDelayLength);
    payloadWriter.bits(0, 4); // pic_struct: frame
    writeFullTimestamp(payloadWriter, 10, 11, 12, 13);
    payloadWriter.bits(0, constructedSyntax.timeOffsetLength);
    if (payloadWriter.bitPosition != 0) payloadWriter.payloadTrailingBits();
    const QByteArray payload = payloadWriter.bytes;
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

void TestH26xSeiTimecode::h264ClockTimestampsAreNondecreasing_data() {
    QTest::addColumn<QByteArray>("payload");
    QTest::addColumn<int>("timeOffsetLength");
    QTest::addColumn<int>("expectedStatus");
    QTest::addColumn<int>("expectedHours");

    using Status = H26xTimingDetail::TimecodeParseStatus;
    const auto payload = [](int picStruct, auto writeClocks) {
        BitWriter writer;
        writer.bits(uint32_t(picStruct), 4);
        writeClocks(writer);
        if (writer.bitPosition != 0) writer.payloadTrailingBits();
        return writer.bytes;
    };

    QTest::newRow("three clocks equal then increasing") << payload(5, [](BitWriter& writer) {
        writeFullTimestamp(writer, 1, 2, 3, 4);
        writeFullTimestamp(writer, 1, 2, 3, 4);
        writeFullTimestamp(writer, 1, 2, 3, 5);
    }) << 0 << int(Status::Valid) << 1;
    QTest::newRow("complete clocks reversed") << payload(3, [](BitWriter& writer) {
        writeFullTimestamp(writer, 1, 2, 3, 5);
        writeFullTimestamp(writer, 1, 2, 3, 4);
    }) << 0 << int(Status::Malformed) << -1;
    QTest::newRow("current discontinuity does not permit reversal")
        << payload(3,
                   [](BitWriter& writer) {
                       writeFullTimestamp(writer, 1, 2, 3, 5);
                       writeFullTimestamp(writer, 1, 2, 3, 4, 0, false, true, 0, 0, true);
                   })
        << 0 << int(Status::Malformed) << -1;
    QTest::newRow("middle discontinuity does not reset three-clock ordering")
        << payload(5,
                   [](BitWriter& writer) {
                       writeFullTimestamp(writer, 1, 2, 3, 8);
                       writeFullTimestamp(writer, 1, 2, 3, 4, 0, false, true, 0, 0, true);
                       writeFullTimestamp(writer, 1, 2, 3, 5);
                   })
        << 0 << int(Status::Malformed) << -1;
    QTest::newRow("third current discontinuity does not permit reversal")
        << payload(5,
                   [](BitWriter& writer) {
                       writeFullTimestamp(writer, 1, 2, 3, 4);
                       writeFullTimestamp(writer, 1, 2, 3, 5);
                       writeFullTimestamp(writer, 1, 2, 3, 3, 0, false, true, 0, 0, true);
                   })
        << 0 << int(Status::Malformed) << -1;
    QTest::newRow("increasing three clocks allow middle discontinuity")
        << payload(5,
                   [](BitWriter& writer) {
                       writeFullTimestamp(writer, 1, 2, 3, 4);
                       writeFullTimestamp(writer, 1, 2, 3, 5, 0, false, true, 0, 0, true);
                       writeFullTimestamp(writer, 1, 2, 3, 6);
                   })
        << 0 << int(Status::Valid) << 1;
    QTest::newRow("type zero offset cannot compensate reversal")
        << payload(3,
                   [](BitWriter& writer) {
                       writeFullTimestamp(writer, 0, 0, 0, 5, 0, false, true, -4, 4);
                       writeFullTimestamp(writer, 0, 0, 0, 4, 0, false, true, 0, 4);
                   })
        << 4 << int(Status::Malformed) << -1;
    QTest::newRow("type one offset compensates reversal") << payload(3, [](BitWriter& writer) {
        writeFullTimestamp(writer, 0, 0, 0, 5, 1, false, true, -4, 4);
        writeFullTimestamp(writer, 0, 0, 0, 4, 1, false, true, 0, 4);
    }) << 4 << int(Status::Valid) << 0;
    QTest::newRow("signed offsets reverse increasing labels") << payload(3, [](BitWriter& writer) {
        writeFullTimestamp(writer, 0, 0, 0, 0, 1, false, true, 2, 4);
        writeFullTimestamp(writer, 0, 0, 0, 1, 1, false, true, -1, 4);
    }) << 4 << int(Status::Malformed) << -1;
    QTest::newRow("field basis reverses equal labels") << payload(3, [](BitWriter& writer) {
        writeFullTimestamp(writer, 0, 0, 0, 1, 1, false, true);
        writeFullTimestamp(writer, 0, 0, 0, 1, 1, false, false);
    }) << 0 << int(Status::Malformed) << -1;
    QTest::newRow("unadjusted day wrap decreases") << payload(3, [](BitWriter& writer) {
        writeFullTimestamp(writer, 23, 59, 59, 24, 1);
        writeFullTimestamp(writer, 0, 0, 0, 0, 1);
    }) << 0 << int(Status::Malformed) << -1;
    QTest::newRow("day wrap offset remains nondecreasing") << payload(3, [](BitWriter& writer) {
        writeFullTimestamp(writer, 23, 59, 59, 24, 1, false, true, 0, 24);
        writeFullTimestamp(writer, 0, 0, 0, 0, 1, false, true, 4'320'000, 24);
    }) << 24 << int(Status::Valid) << 23;
}

void TestH26xSeiTimecode::h264SeiMessagesAreFullyValidated() {
    const QByteArray reference = fixture("h264_pic_timing_no_hrd.264");
    H26xTimingContext context;
    QVERIFY(
        context.updateParameterSets(NativeVideoCodec::H264, {}, {h264SpsFromAnnexB(reference)}));

    const QByteArray valid = seiMessage(1, fullTimestampPayload(1, 2, 3, 4));
    const QByteArray truncated =
        valid + seiVarByte(1) + seiVarByte(4) + QByteArray::fromHex("0a0b");
    QVERIFY(
        !extractH26xSeiTimecode(rawH264SeiNal(truncated), NativeVideoCodec::H264, context).valid);

    BitWriter reservedCountingType;
    reservedCountingType.bits(0, 4);
    writeFullTimestamp(reservedCountingType, 1, 2, 3, 5, 7);
    reservedCountingType.payloadTrailingBits();
    const QByteArray malformedType = valid + seiMessage(1, reservedCountingType.bytes);
    QVERIFY(
        !extractH26xSeiTimecode(h264SeiNal(malformedType), NativeVideoCodec::H264, context).valid);

    QVERIFY(!extractH26xSeiTimecode(rawH264SeiNal(valid), NativeVideoCodec::H264, context).valid);
    QVERIFY(!extractH26xSeiTimecode(rawH264SeiNal(valid + QByteArray(1, char(0x81))),
                                    NativeVideoCodec::H264, context)
                 .valid);

    const QByteArray validFirstNal = h264SeiNal(valid);
    QVERIFY(!extractH26xSeiTimecode(validFirstNal + rawH264SeiNal(QByteArray::fromHex("0104aabb")),
                                    NativeVideoCodec::H264, context)
                 .valid);

    for (const int zeroCount : {1, 2, 9}) {
        const auto parsed =
            extractH26xSeiTimecode(h264SeiNal(valid + seiMessage(5, QByteArray::fromHex("aabb"))) +
                                       QByteArray(zeroCount, char(0)),
                                   NativeVideoCodec::H264, context);
        QVERIFY2(parsed.valid,
                 qPrintable(QStringLiteral("multi-message trailing zeros=%1").arg(zeroCount)));
        QCOMPARE(parsed.frames, 4);
    }
}

void TestH26xSeiTimecode::h264SeiMessagesAggregateTypedResults() {
    const QByteArray reference = fixture("h264_pic_timing_no_hrd.264");
    H26xTimingContext context;
    QVERIFY(
        context.updateParameterSets(NativeVideoCodec::H264, {}, {h264SpsFromAnnexB(reference)}));

    const QByteArray first = seiMessage(1, fullTimestampPayload(1, 2, 3, 4));
    const QByteArray later = seiMessage(1, fullTimestampPayload(2, 3, 4, 5));
    auto parsed =
        extractH26xSeiTimecode(h264SeiNal(first + later), NativeVideoCodec::H264, context);
    QVERIFY(parsed.valid);
    QCOMPARE(parsed.hours, 1);
    QCOMPARE(parsed.minutes, 2);
    QCOMPARE(parsed.seconds, 3);
    QCOMPARE(parsed.frames, 4);

    parsed =
        extractH26xSeiTimecode(h264SeiNal(first + seiMessage(5, QByteArray::fromHex("aabbcc"))),
                               NativeVideoCodec::H264, context);
    QVERIFY(parsed.valid);
    QCOMPARE(parsed.frames, 4);

    const QByteArray unsupported = seiMessage(1, fullTimestampPayload(1, 2, 3, 5, 5));
    QVERIFY(
        !extractH26xSeiTimecode(h264SeiNal(first + unsupported), NativeVideoCodec::H264, context)
             .valid);
    QVERIFY(
        !extractH26xSeiTimecode(h264SeiNal(unsupported + later), NativeVideoCodec::H264, context)
             .valid);
}

void TestH26xSeiTimecode::h264ClockTimestampsAreNondecreasing() {
    QFETCH(QByteArray, payload);
    QFETCH(int, timeOffsetLength);
    QFETCH(int, expectedStatus);
    QFETCH(int, expectedHours);

    const QByteArray reference = fixture("h264_pic_timing_no_hrd.264");
    H26xTimingContext context;
    QVERIFY(
        context.updateParameterSets(NativeVideoCodec::H264, {}, {h264SpsFromAnnexB(reference)}));
    H264TimingSyntax syntax = *context.h264();
    syntax.timeOffsetLength = uint8_t(timeOffsetLength);

    const auto parsed = H26xTimingDetail::parseH264PicTiming(payload, syntax);
    QCOMPARE(int(parsed.status), expectedStatus);
    QCOMPARE(parsed.timecode.valid,
             expectedStatus == int(H26xTimingDetail::TimecodeParseStatus::Valid));
    if (parsed.timecode.valid) QCOMPARE(parsed.timecode.hours, expectedHours);
}

void TestH26xSeiTimecode::h264ConsecutiveFieldTimestampsRespectCtType_data() {
    QTest::addColumn<int>("picStruct");
    QTest::addColumn<int>("firstCtType");
    QTest::addColumn<int>("secondCtType");
    QTest::addColumn<bool>("equalClockTimestamps");
    QTest::addColumn<int>("expectedStatus");

    using Status = H26xTimingDetail::TimecodeParseStatus;
    for (const int picStruct : {3, 4, 5, 6}) {
        const auto row = [picStruct](const char* description, int firstCtType, int secondCtType,
                                     bool equal, Status status) {
            QTest::newRow(qPrintable(QStringLiteral("pic_struct %1 %2")
                                         .arg(picStruct)
                                         .arg(QString::fromLatin1(description))))
                << picStruct << firstCtType << secondCtType << equal << int(status);
        };
        row("equal progressive", 0, 0, true, Status::Valid);
        row("equal progressive unknown", 0, 2, true, Status::Valid);
        row("equal unknown", 2, 2, true, Status::Valid);
        row("equal second interlaced", 0, 1, true, Status::Malformed);
        row("equal first interlaced", 1, 0, true, Status::Malformed);
        row("equal both interlaced", 1, 1, true, Status::Malformed);
        row("different second interlaced", 0, 1, false, Status::Valid);
        row("different first interlaced", 1, 0, false, Status::Valid);
    }
    QTest::newRow("pic_struct 7 equal interlaced frame timestamps")
        << 7 << 1 << 1 << true << int(Status::Valid);
}

void TestH26xSeiTimecode::h264ConsecutiveFieldTimestampsRespectCtType() {
    QFETCH(int, picStruct);
    QFETCH(int, firstCtType);
    QFETCH(int, secondCtType);
    QFETCH(bool, equalClockTimestamps);
    QFETCH(int, expectedStatus);

    H264TimingSyntax syntax;
    syntax.status = H26xTimingSyntaxStatus::Valid;
    syntax.frameRate = {25, 1};
    syntax.numUnitsInTick = 1;
    syntax.timeScale = 50;
    syntax.fixedFrameRate = true;
    syntax.picStructPresent = true;

    BitWriter writer;
    writer.bits(uint32_t(picStruct), 4);
    writeFullTimestamp(writer, 1, 2, 3, 4, 0, false, true, 0, 0, false, firstCtType);
    writeFullTimestamp(writer, 1, 2, 3, equalClockTimestamps ? 4 : 5, 0, false, true, 0, 0, false,
                       secondCtType);
    if (picStruct == 5 || picStruct == 6)
        writeFullTimestamp(writer, 1, 2, 3, equalClockTimestamps ? 5 : 6);
    if (writer.bitPosition != 0) writer.payloadTrailingBits();

    const auto parsed = H26xTimingDetail::parseH264PicTiming(writer.bytes, syntax);
    QCOMPARE(int(parsed.status), expectedStatus);
    QCOMPARE(parsed.timecode.valid,
             expectedStatus == int(H26xTimingDetail::TimecodeParseStatus::Valid));
}

void TestH26xSeiTimecode::h264CountingSchemesApplyOffsetsBeforeClassification_data() {
    QTest::addColumn<int>("countingType");
    QTest::addColumn<bool>("reversedLabels");
    QTest::addColumn<int>("firstOffset");
    QTest::addColumn<int>("secondOffset");
    QTest::addColumn<int>("expectedStatus");

    using Status = H26xTimingDetail::TimecodeParseStatus;
    QTest::newRow("type 4 offset compensates reversed 29.97 DF labels")
        << 4 << true << -2002 << 0 << int(Status::Valid);
    for (const int countingType : {2, 3, 5, 6}) {
        QTest::newRow(
            qPrintable(QStringLiteral("type %1 offset reversal is malformed").arg(countingType)))
            << countingType << false << 2003 << 0 << int(Status::Malformed);
    }
}

void TestH26xSeiTimecode::h264CountingSchemesApplyOffsetsBeforeClassification() {
    QFETCH(int, countingType);
    QFETCH(bool, reversedLabels);
    QFETCH(int, firstOffset);
    QFETCH(int, secondOffset);
    QFETCH(int, expectedStatus);

    H264TimingSyntax syntax;
    syntax.status = H26xTimingSyntaxStatus::Valid;
    syntax.frameRate = {30000, 1001};
    syntax.numUnitsInTick = 1001;
    syntax.timeScale = 60000;
    syntax.fixedFrameRate = true;
    syntax.timeOffsetLength = 12;
    syntax.picStructPresent = true;

    BitWriter writer;
    writer.bits(3, 4); // pic_struct: top field, bottom field
    writeFullTimestamp(writer, 0, 0, 0, reversedLabels ? 5 : 0, countingType, false, true,
                       firstOffset, syntax.timeOffsetLength);
    writeFullTimestamp(writer, 0, 0, 0, reversedLabels ? 4 : 1, countingType, false, true,
                       secondOffset, syntax.timeOffsetLength);
    if (writer.bitPosition != 0) writer.payloadTrailingBits();

    const auto parsed = H26xTimingDetail::parseH264PicTiming(writer.bytes, syntax);
    QCOMPARE(int(parsed.status), expectedStatus);
    QCOMPARE(parsed.timecode.valid,
             expectedStatus == int(H26xTimingDetail::TimecodeParseStatus::Valid));
    if (parsed.timecode.valid) QVERIFY(parsed.timecode.dropFrame);
}

void TestH26xSeiTimecode::h264NonCfrTimingIsValidatedBeforeUnsupported() {
    H264TimingSyntax syntax;
    syntax.status = H26xTimingSyntaxStatus::Valid;
    syntax.frameRate = {30000, 1001};
    syntax.numUnitsInTick = 1001;
    syntax.timeScale = 60000;
    syntax.fixedFrameRate = false;
    syntax.picStructPresent = true;

    auto parsed = H26xTimingDetail::parseH264PicTiming(fullTimestampPayload(1, 2, 3, 4), syntax);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Unsupported);
    QVERIFY(!parsed.timecode.valid);

    parsed = H26xTimingDetail::parseH264PicTiming(fullTimestampPayload(1, 2, 3, 30), syntax);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Malformed);

    parsed = H26xTimingDetail::parseH264PicTiming(fullTimestampPayload(0, 1, 0, 0, 4, false, true),
                                                  syntax);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Malformed);

    BitWriter reversedClocks;
    reversedClocks.bits(3, 4);
    writeFullTimestamp(reversedClocks, 1, 2, 3, 5);
    writeFullTimestamp(reversedClocks, 1, 2, 3, 4);
    if (reversedClocks.bitPosition != 0) reversedClocks.payloadTrailingBits();
    parsed = H26xTimingDetail::parseH264PicTiming(reversedClocks.bytes, syntax);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Malformed);
}

void TestH26xSeiTimecode::h264LaterIncompleteClockInheritsUnits_data() {
    QTest::addColumn<int>("seconds");
    QTest::addColumn<int>("expectedStatus");

    using Status = H26xTimingDetail::TimecodeParseStatus;
    QTest::newRow("inherits all units and advances frame") << -1 << int(Status::Valid);
    QTest::newRow("inherits minute and hour but decreases second") << 2 << int(Status::Malformed);
    QTest::newRow("inherits minute and hour and advances second") << 4 << int(Status::Valid);
}

void TestH26xSeiTimecode::h264LaterIncompleteClockInheritsUnits() {
    QFETCH(int, seconds);
    QFETCH(int, expectedStatus);

    BitWriter writer;
    writer.bits(3, 4); // pic_struct: top field, bottom field
    writeFullTimestamp(writer, 1, 2, 3, 4);
    writer.bit(true);  // clock_timestamp_flag[1]
    writer.bits(0, 2); // ct_type
    writer.bit(true);  // nuit_field_based_flag
    writer.bits(0, 5); // counting_type
    writer.bit(false); // full_timestamp_flag
    writer.bit(false); // discontinuity_flag
    writer.bit(false); // cnt_dropped_flag
    writer.bits(5, 8); // n_frames
    writer.bit(seconds >= 0);
    if (seconds >= 0) {
        writer.bits(uint32_t(seconds), 6);
        writer.bit(false); // inherit minutes_value and hours_value
    }
    if (writer.bitPosition != 0) writer.payloadTrailingBits();

    const QByteArray reference = fixture("h264_pic_timing_no_hrd.264");
    H26xTimingContext context;
    QVERIFY(
        context.updateParameterSets(NativeVideoCodec::H264, {}, {h264SpsFromAnnexB(reference)}));
    const auto parsed = H26xTimingDetail::parseH264PicTiming(writer.bytes, *context.h264());
    QCOMPARE(int(parsed.status), expectedStatus);
    QCOMPARE(parsed.timecode.valid,
             expectedStatus == int(H26xTimingDetail::TimecodeParseStatus::Valid));
    if (parsed.timecode.valid) {
        QCOMPARE(parsed.timecode.hours, 1);
        QCOMPARE(parsed.timecode.minutes, 2);
        QCOMPARE(parsed.timecode.seconds, 3);
        QCOMPARE(parsed.timecode.frames, 4);
    }
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
    struct Case {
        FrameRateQ rate;
        int countingType;
        bool countDropped;
        bool nuitFieldBased;
        int minutes;
        int seconds;
        int frames;
        H26xTimingDetail::TimecodeParseStatus status;
        bool dropFrame;
    };
    const Case cases[] = {
        {{25, 1}, 0, false, true, 1, 0, 0, H26xTimingDetail::TimecodeParseStatus::Valid, false},
        {{25, 1}, 0, true, true, 1, 0, 0, H26xTimingDetail::TimecodeParseStatus::Malformed, false},
        {{25, 1}, 1, false, true, 1, 0, 0, H26xTimingDetail::TimecodeParseStatus::Valid, false},
        {{25, 1}, 1, true, true, 1, 0, 0, H26xTimingDetail::TimecodeParseStatus::Malformed, false},
        {{25, 1},
         0,
         false,
         false,
         1,
         0,
         0,
         H26xTimingDetail::TimecodeParseStatus::Unsupported,
         false},
        {{30000, 1001},
         2,
         false,
         true,
         1,
         0,
         0,
         H26xTimingDetail::TimecodeParseStatus::Unsupported,
         false},
        {{30000, 1001},
         2,
         true,
         true,
         1,
         0,
         1,
         H26xTimingDetail::TimecodeParseStatus::Unsupported,
         false},
        {{30000, 1001},
         2,
         true,
         true,
         1,
         0,
         2,
         H26xTimingDetail::TimecodeParseStatus::Malformed,
         false},
        {{30000, 1001},
         3,
         false,
         true,
         1,
         0,
         0,
         H26xTimingDetail::TimecodeParseStatus::Unsupported,
         false},
        {{30000, 1001},
         3,
         true,
         true,
         1,
         0,
         0,
         H26xTimingDetail::TimecodeParseStatus::Unsupported,
         false},
        {{30000, 1001},
         3,
         true,
         true,
         1,
         0,
         1,
         H26xTimingDetail::TimecodeParseStatus::Malformed,
         false},
        // Type 4 is the NTSC drop-frame scheme even when this timestamp did not skip.
        {{30000, 1001},
         4,
         false,
         true,
         1,
         1,
         0,
         H26xTimingDetail::TimecodeParseStatus::Valid,
         true},
        // The two forbidden minute-boundary labels stay illegal independently of cnt_dropped_flag.
        {{30000, 1001},
         4,
         false,
         true,
         1,
         0,
         0,
         H26xTimingDetail::TimecodeParseStatus::Malformed,
         false},
        {{30000, 1001},
         4,
         false,
         true,
         1,
         0,
         1,
         H26xTimingDetail::TimecodeParseStatus::Malformed,
         false},
        {{30000, 1001}, 4, true, true, 1, 0, 2, H26xTimingDetail::TimecodeParseStatus::Valid, true},
        // Tenth-minute boundaries do not skip and frame 00 is legal drop-frame timecode.
        {{30000, 1001},
         4,
         false,
         true,
         10,
         0,
         0,
         H26xTimingDetail::TimecodeParseStatus::Valid,
         true},
        {{30000, 1001},
         4,
         true,
         true,
         0,
         0,
         2,
         H26xTimingDetail::TimecodeParseStatus::Malformed,
         false},
        {{30000, 1001},
         4,
         true,
         true,
         1,
         1,
         2,
         H26xTimingDetail::TimecodeParseStatus::Malformed,
         false},
        {{30000, 1001},
         4,
         true,
         true,
         1,
         0,
         3,
         H26xTimingDetail::TimecodeParseStatus::Malformed,
         false},
        // The type-4 two-label skip is not SMPTE 59.94 DF's four-label scheme.
        {{60000, 1001},
         4,
         false,
         true,
         1,
         1,
         0,
         H26xTimingDetail::TimecodeParseStatus::Unsupported,
         false},
        {{30000, 1001},
         4,
         false,
         false,
         1,
         1,
         0,
         H26xTimingDetail::TimecodeParseStatus::Unsupported,
         false},
        {{30000, 1001},
         5,
         false,
         true,
         1,
         0,
         0,
         H26xTimingDetail::TimecodeParseStatus::Unsupported,
         false},
        {{30000, 1001},
         5,
         true,
         true,
         1,
         0,
         2,
         H26xTimingDetail::TimecodeParseStatus::Unsupported,
         false},
        {{30000, 1001},
         6,
         false,
         true,
         1,
         0,
         0,
         H26xTimingDetail::TimecodeParseStatus::Unsupported,
         false},
        {{30000, 1001},
         6,
         true,
         true,
         1,
         0,
         2,
         H26xTimingDetail::TimecodeParseStatus::Unsupported,
         false},
        {{30000, 1001},
         7,
         false,
         true,
         1,
         0,
         0,
         H26xTimingDetail::TimecodeParseStatus::Malformed,
         false},
        {{30000, 1001},
         31,
         true,
         true,
         1,
         0,
         0,
         H26xTimingDetail::TimecodeParseStatus::Malformed,
         false},
    };

    for (const Case& test : cases) {
        H264TimingSyntax syntax;
        syntax.status = H26xTimingSyntaxStatus::Valid;
        syntax.frameRate = test.rate;
        syntax.fixedFrameRate = true;
        syntax.picStructPresent = true;
        const auto parsed = H26xTimingDetail::parseH264PicTiming(
            fullTimestampPayload(1, test.minutes, test.seconds, test.frames, test.countingType,
                                 test.countDropped, test.nuitFieldBased),
            syntax);
        QCOMPARE(parsed.status, test.status);
        QCOMPARE(parsed.timecode.valid,
                 test.status == H26xTimingDetail::TimecodeParseStatus::Valid);
        if (parsed.timecode.valid) QCOMPARE(parsed.timecode.dropFrame, test.dropFrame);
    }

    H264TimingSyntax syntax;
    syntax.status = H26xTimingSyntaxStatus::Valid;
    syntax.frameRate = {30000, 1001};
    syntax.fixedFrameRate = true;
    syntax.picStructPresent = true;
    BitWriter inheritedDropUnits;
    inheritedDropUnits.bits(0, 4);
    inheritedDropUnits.bit(true);  // clock_timestamp_flag
    inheritedDropUnits.bits(0, 2); // ct_type
    inheritedDropUnits.bit(true);  // nuit_field_based_flag
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

void TestH26xSeiTimecode::h264ValidatesPartialTimestampFields_data() {
    QTest::addColumn<int>("rateNum");
    QTest::addColumn<int>("rateDen");
    QTest::addColumn<int>("frames");
    QTest::addColumn<int>("seconds");
    QTest::addColumn<int>("minutes");
    QTest::addColumn<int>("hours");
    QTest::addColumn<int>("expectedStatus");

    using Status = H26xTimingDetail::TimecodeParseStatus;
    QTest::newRow("no-units") << 25 << 1 << 4 << -1 << -1 << -1 << int(Status::NoTimestamp);
    QTest::newRow("seconds-only") << 25 << 1 << 4 << 3 << -1 << -1 << int(Status::NoTimestamp);
    QTest::newRow("minutes-no-hours") << 25 << 1 << 4 << 3 << 2 << -1 << int(Status::NoTimestamp);
    QTest::newRow("seconds-out-of-range")
        << 25 << 1 << 4 << 60 << -1 << -1 << int(Status::Malformed);
    QTest::newRow("minutes-out-of-range")
        << 25 << 1 << 4 << 3 << 60 << -1 << int(Status::Malformed);
    QTest::newRow("hours-out-of-range") << 25 << 1 << 4 << 3 << 2 << 24 << int(Status::Malformed);
    QTest::newRow("integer-rate-frame-out-of-range")
        << 25 << 1 << 25 << -1 << -1 << -1 << int(Status::Malformed);
    QTest::newRow("fractional-rate-last-frame")
        << 30000 << 1001 << 29 << -1 << -1 << -1 << int(Status::NoTimestamp);
    QTest::newRow("fractional-rate-frame-out-of-range")
        << 30000 << 1001 << 30 << -1 << -1 << -1 << int(Status::Malformed);
}

void TestH26xSeiTimecode::h264ValidatesPartialTimestampFields() {
    QFETCH(int, rateNum);
    QFETCH(int, rateDen);
    QFETCH(int, frames);
    QFETCH(int, seconds);
    QFETCH(int, minutes);
    QFETCH(int, hours);
    QFETCH(int, expectedStatus);

    H264TimingSyntax syntax;
    syntax.status = H26xTimingSyntaxStatus::Valid;
    syntax.frameRate = {rateNum, rateDen};
    syntax.fixedFrameRate = true;
    syntax.picStructPresent = true;
    const auto parsed = H26xTimingDetail::parseH264PicTiming(
        partialTimestampPayload(frames, seconds, minutes, hours), syntax);
    QCOMPARE(int(parsed.status), expectedStatus);
    QVERIFY(!parsed.timecode.valid);
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

    const QByteArray separatedNals = QByteArray(kStartCode4, 4) + QByteArray::fromHex("06050080") +
                                     QByteArray::fromHex("000001") + QByteArray(1, char(0x06)) +
                                     escapeRbsp(validMessage + QByteArray(1, char(0x80)));
    const auto splitResult = extractH26xSeiTimecode(separatedNals, NativeVideoCodec::H264, context);
    QVERIFY(splitResult.valid);
    QCOMPARE(splitResult.frames, 4);
}

void TestH26xSeiTimecode::h264RejectsInvalidSeiNalHeaders_data() {
    QTest::addColumn<int>("header");
    QTest::addColumn<int>("position");

    for (const int header : {0x86, 0x26}) {
        const QString headerName = QString::number(header, 16);
        QTest::newRow(qPrintable(QStringLiteral("0x%1 only").arg(headerName))) << header << 0;
        QTest::newRow(qPrintable(QStringLiteral("0x%1 before valid").arg(headerName)))
            << header << 1;
        QTest::newRow(qPrintable(QStringLiteral("0x%1 after valid").arg(headerName)))
            << header << 2;
    }
}

void TestH26xSeiTimecode::h264RejectsInvalidSeiNalHeaders() {
    QFETCH(int, header);
    QFETCH(int, position);

    const QByteArray reference = fixture("h264_pic_timing_no_hrd.264");
    H26xTimingContext context;
    QVERIFY(
        context.updateParameterSets(NativeVideoCodec::H264, {}, {h264SpsFromAnnexB(reference)}));

    const QByteArray message = seiMessage(1, fullTimestampPayload(1, 2, 3, 4));
    const QByteArray invalid = seiNal(QByteArray(1, char(header)), message);
    const QByteArray valid = h264SeiNal(message);
    QByteArray annexB = invalid;
    if (position == 1) annexB += valid;
    if (position == 2) annexB = valid + invalid;

    QVERIFY(!extractH26xSeiTimecode(annexB, NativeVideoCodec::H264, context).valid);
}

void TestH26xSeiTimecode::h264AnnexBTrailingZeroBytesAreNotNalPayload() {
    const QByteArray reference = fixture("h264_pic_timing_no_hrd.264");
    H26xTimingContext context;
    QVERIFY(
        context.updateParameterSets(NativeVideoCodec::H264, {}, {h264SpsFromAnnexB(reference)}));

    const QByteArray validMessage = seiMessage(1, fullTimestampPayload(1, 2, 3, 4));
    const QByteArray sei = h264SeiNal(validMessage);
    for (const int zeroCount : {1, 2, 9}) {
        const QByteArray zeros(zeroCount, char(0));

        const auto finalNal = extractH26xSeiTimecode(sei + zeros, NativeVideoCodec::H264, context);
        QVERIFY2(finalNal.valid, qPrintable(QStringLiteral("final zeros=%1").arg(zeroCount)));
        QCOMPARE(finalNal.frames, 4);

        const auto beforeNext =
            extractH26xSeiTimecode(sei + zeros + h264VclNal(), NativeVideoCodec::H264, context);
        QVERIFY2(beforeNext.valid,
                 qPrintable(QStringLiteral("zeros before next NAL=%1").arg(zeroCount)));
        QCOMPARE(beforeNext.frames, 4);
    }

    // Zeros inside a declared payload precede rbsp_trailing_bits and are NAL data,
    // not Annex-B padding. They must survive splitting and EBSP decoding unchanged.
    const QByteArray leadingPayloadWithZeros = seiMessage(5, QByteArray::fromHex("120000"));
    const auto payloadZeros = extractH26xSeiTimecode(
        h264SeiNal(leadingPayloadWithZeros + validMessage), NativeVideoCodec::H264, context);
    QVERIFY(payloadZeros.valid);
    QCOMPARE(payloadZeros.frames, 4);
}

void TestH26xSeiTimecode::rawEbspTrailingZeroBytesRemainStrict() {
    QByteArray decoded;
    QVERIFY(H26xTimingDetail::unescapeRbsp(QByteArray::fromHex("128000"), decoded));
    QCOMPARE(decoded, QByteArray::fromHex("128000"));

    QVERIFY(H26xTimingDetail::unescapeRbsp(QByteArray::fromHex("12800000"), decoded));
    QCOMPARE(decoded, QByteArray::fromHex("12800000"));

    QVERIFY(!H26xTimingDetail::unescapeRbsp(QByteArray::fromHex("1280000000"), decoded));
    QVERIFY(decoded.isEmpty());
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
