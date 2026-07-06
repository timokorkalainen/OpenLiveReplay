#include <QtTest>

#include "recorder_engine/ingest/colorvui.h"
#include "recorder_engine/codec/colorvui.h"

class TestColorVui : public QObject {
    Q_OBJECT
private slots:
    void noVuiYieldsAbsent();
    void bt709FullRangeIsParsed();
    void bt601VideoRangeIsParsed();
    void tooShortNalIsAbsent();
    void encoderBt709VideoRangeIsOneOneOne();
    void encoderBt601VideoRangeIsSixSixSix();
    void encoderBt2020FullRange();
    void encoderUnspecifiedPrimariesAndTransferAreTwo();
    void parserTransferCodeMapping();
    void encoderBt601CodePointsRoundTripThroughSpsParser();
    void encoderBt2020CodePointsRoundTripThroughSpsParser();

private:
    static QByteArray makeSps(bool withColour, int primaries, int transfer, int matrix,
                              bool fullRange);
};

namespace {
struct BitWriter {
    QByteArray bytes;
    int bitPos = 0;

    void putBit(int b) {
        if (bitPos == 0) bytes.append(char(0));
        if (b) bytes[bytes.size() - 1] = char(quint8(bytes.back()) | (1 << (7 - bitPos)));
        bitPos = (bitPos + 1) & 7;
    }

    void putBits(unsigned v, int n) {
        for (int i = n - 1; i >= 0; --i)
            putBit((v >> i) & 1);
    }

    void ue(unsigned v) {
        unsigned x = v + 1;
        int n = 0;
        while (x >> (n + 1))
            ++n;
        for (int i = 0; i < n; ++i)
            putBit(0);
        for (int i = n; i >= 0; --i)
            putBit((x >> i) & 1);
    }
};
} // namespace

QByteArray TestColorVui::makeSps(bool withColour, int primaries, int transfer, int matrix,
                                 bool fullRange) {
    BitWriter w;
    w.putBits(0x67, 8);
    w.putBits(66, 8);
    w.putBits(0, 8);
    w.putBits(31, 8);
    w.ue(0);
    w.ue(0);
    w.ue(0);
    w.ue(0);
    w.ue(1);
    w.putBit(0);
    w.ue(7);
    w.ue(7);
    w.putBit(1);
    w.putBit(0);
    w.putBit(0);
    w.putBit(1);
    w.putBit(0);
    w.putBit(0);
    if (withColour) {
        w.putBit(1);
        w.putBits(5, 3);
        w.putBit(fullRange ? 1 : 0);
        w.putBit(1);
        w.putBits(unsigned(primaries), 8);
        w.putBits(unsigned(transfer), 8);
        w.putBits(unsigned(matrix), 8);
    } else {
        w.putBit(0);
    }
    w.putBit(0);
    w.putBit(0);
    w.putBit(0);
    w.putBit(0);
    w.putBit(0);
    w.putBit(0);
    w.putBit(1);
    return w.bytes;
}

void TestColorVui::noVuiYieldsAbsent() {
    const QByteArray sps = makeSps(false, 0, 0, 0, false);
    const VuiColorInfo info = parseSpsColorVui(NativeVideoCodec::H264, sps);
    QVERIFY(!info.present);
}

void TestColorVui::bt709FullRangeIsParsed() {
    const QByteArray sps = makeSps(true, 1, 1, 1, true);
    const VuiColorInfo info = parseSpsColorVui(NativeVideoCodec::H264, sps);
    QVERIFY(info.present);
    QCOMPARE(int(info.primaries), int(ColorPrimaries::Bt709));
    QCOMPARE(int(info.transfer), int(ColorTransfer::Bt709));
    QCOMPARE(int(info.matrix), int(ColorMatrix::Bt709));
    QCOMPARE(int(info.range), int(ColorRange::Full));
}

void TestColorVui::bt601VideoRangeIsParsed() {
    const QByteArray sps = makeSps(true, 6, 6, 6, false);
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

void TestColorVui::encoderBt709VideoRangeIsOneOneOne() {
    ColorMetadata c;
    const VuiColorCodePoints v = vuiColorCodePointsFor(c);
    QCOMPARE(v.colourPrimaries, 1);
    QCOMPARE(v.transferCharacteristics, 1);
    QCOMPARE(v.matrixCoefficients, 1);
    QCOMPARE(v.fullRange, false);
}

void TestColorVui::encoderBt601VideoRangeIsSixSixSix() {
    ColorMetadata c;
    c.matrix = ColorMatrix::Bt601;
    c.primaries = ColorPrimaries::Bt601;
    c.transfer = ColorTransfer::Bt601;
    const VuiColorCodePoints v = vuiColorCodePointsFor(c);
    QCOMPARE(v.colourPrimaries, 6);
    QCOMPARE(v.transferCharacteristics, 6);
    QCOMPARE(v.matrixCoefficients, 6);
    QCOMPARE(v.fullRange, false);
}

void TestColorVui::encoderBt2020FullRange() {
    ColorMetadata c;
    c.matrix = ColorMatrix::Bt2020;
    c.primaries = ColorPrimaries::Bt2020;
    c.transfer = ColorTransfer::Bt2020;
    c.range = ColorRange::Full;
    const VuiColorCodePoints v = vuiColorCodePointsFor(c);
    QCOMPARE(v.colourPrimaries, 9);
    QCOMPARE(v.transferCharacteristics, 14);
    QCOMPARE(v.matrixCoefficients, 9);
    QCOMPARE(v.fullRange, true);
}

void TestColorVui::encoderUnspecifiedPrimariesAndTransferAreTwo() {
    ColorMetadata c;
    c.primaries = ColorPrimaries::Unspecified;
    c.transfer = ColorTransfer::Unspecified;
    const VuiColorCodePoints v = vuiColorCodePointsFor(c);
    QCOMPARE(v.colourPrimaries, 2);
    QCOMPARE(v.transferCharacteristics, 2);
}

void TestColorVui::parserTransferCodeMapping() {
    const struct Case {
        int code;
        ColorTransfer expected;
    } cases[] = {{5, ColorTransfer::Bt601},   {6, ColorTransfer::Bt601},
                 {8, ColorTransfer::Bt601},   {14, ColorTransfer::Bt2020},
                 {15, ColorTransfer::Bt2020}, {16, ColorTransfer::Bt2020}};

    for (const Case& c : cases) {
        const VuiColorInfo info =
            parseSpsColorVui(NativeVideoCodec::H264, makeSps(true, 1, c.code, 1, false));
        QVERIFY(info.present);
        QCOMPARE(int(info.transfer), int(c.expected));
    }
}

void TestColorVui::encoderBt601CodePointsRoundTripThroughSpsParser() {
    ColorMetadata c;
    c.matrix = ColorMatrix::Bt601;
    c.primaries = ColorPrimaries::Bt601;
    c.transfer = ColorTransfer::Bt601;
    const VuiColorCodePoints v = vuiColorCodePointsFor(c);
    const VuiColorInfo info = parseSpsColorVui(
        NativeVideoCodec::H264, makeSps(true, v.colourPrimaries, v.transferCharacteristics,
                                        v.matrixCoefficients, v.fullRange));
    QVERIFY(info.present);
    QCOMPARE(int(info.primaries), int(c.primaries));
    QCOMPARE(int(info.transfer), int(c.transfer));
    QCOMPARE(int(info.matrix), int(c.matrix));
    QCOMPARE(int(info.range), int(c.range));
}

void TestColorVui::encoderBt2020CodePointsRoundTripThroughSpsParser() {
    ColorMetadata c;
    c.matrix = ColorMatrix::Bt2020;
    c.primaries = ColorPrimaries::Bt2020;
    c.transfer = ColorTransfer::Bt2020;
    c.range = ColorRange::Full;
    const VuiColorCodePoints v = vuiColorCodePointsFor(c);
    const VuiColorInfo info = parseSpsColorVui(
        NativeVideoCodec::H264, makeSps(true, v.colourPrimaries, v.transferCharacteristics,
                                        v.matrixCoefficients, v.fullRange));
    QVERIFY(info.present);
    QCOMPARE(int(info.primaries), int(c.primaries));
    QCOMPARE(int(info.transfer), int(c.transfer));
    QCOMPARE(int(info.matrix), int(c.matrix));
    QCOMPARE(int(info.range), int(c.range));
}

QTEST_GUILESS_MAIN(TestColorVui)
#include "tst_colorvui.moc"
