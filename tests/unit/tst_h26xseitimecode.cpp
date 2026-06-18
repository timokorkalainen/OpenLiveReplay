#include <QtTest>

#include "recorder_engine/ingest/h26xseitimecode.h"
#include "recorder_engine/timing/smpte12m.h"

class TestH26xSeiTimecode : public QObject {
    Q_OBJECT

private slots:
    void h264PicTimingDecoded();
    void hevcTimeCodeDecoded();
    void hevcSuffixSeiDecoded();
    void emulationPreventionStripped();
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

} // namespace

void TestH26xSeiTimecode::h264PicTimingDecoded() {
    const Smpte12mTimecode want{10, 11, 12, 13, /*drop*/ false, /*valid*/ true};
    const QByteArray rbsp = seiMessage(/*pic_timing*/ 1, packedWordBytes(want));
    const QByteArray annexB = h264SeiNal(rbsp) + h264VclNal();

    const Smpte12mTimecode got = extractH26xSeiTimecode(annexB, NativeVideoCodec::H264);
    QVERIFY(got.valid);
    QCOMPARE(got.hours, 10);
    QCOMPARE(got.minutes, 11);
    QCOMPARE(got.seconds, 12);
    QCOMPARE(got.frames, 13);
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

void TestH26xSeiTimecode::emulationPreventionStripped() {
    // Choose a TC whose packed word contains a 00 00 0x sequence so the encoder
    // would have inserted an emulation-prevention 0x03. 00:00:00:00 packs to the
    // word 0x00000000, i.e. payload bytes 00 00 00 00 -> emulated as 00 00 03 00
    // 00 03 00 ... We hand-build the emulated stream and require the extractor to
    // strip 00 00 03 -> 00 00 before parsing.
    const Smpte12mTimecode want{0, 0, 0, 0, /*drop*/ false, /*valid*/ true};
    const QByteArray rbspRaw =
        seiMessage(1, packedWordBytes(want)); // type=1,size=4,payload=00000000

    // Emulation-prevention encode rbspRaw: insert 0x03 after every 00 00.
    QByteArray emulated;
    int zeros = 0;
    for (char c : rbspRaw) {
        const uchar v = uchar(c);
        if (zeros >= 2 && v <= 0x03) {
            emulated.append(char(0x03));
            zeros = 0;
        }
        emulated.append(c);
        zeros = (v == 0) ? zeros + 1 : 0;
    }
    QVERIFY(emulated.size() > rbspRaw.size()); // emulation bytes really inserted

    const QByteArray annexB = h264SeiNal(emulated) + h264VclNal();
    const Smpte12mTimecode got = extractH26xSeiTimecode(annexB, NativeVideoCodec::H264);
    QVERIFY(got.valid);
    QCOMPARE(got.hours, 0);
    QCOMPARE(got.minutes, 0);
    QCOMPARE(got.seconds, 0);
    QCOMPARE(got.frames, 0);
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
