#include <QtTest>

#include "playback/output/colormetadatapolicy.h"
#include "recorder_engine/ingest/colorvui.h"

static constexpr int kUnspecified = 2;

class TestColorMetadataPolicy : public QObject {
    Q_OBJECT
private slots:
    void heightThresholdMatchesLegacyConstant();
    void untaggedTallFrameIsBt709VideoNoOp();
    void untaggedShortFrameIsBt601VideoNoOp();
    void vuiOverridesHeightDefault();
    void ffmpegCodesOverrideHeightDefaultWhenNoVui();
};

void TestColorMetadataPolicy::heightThresholdMatchesLegacyConstant() {
    QCOMPARE(kDefaultBt709HeightThreshold, 576);
}

void TestColorMetadataPolicy::untaggedTallFrameIsBt709VideoNoOp() {
    const ColorMetadata m = resolveColorMetadata(VuiColorInfo{}, 1080, kUnspecified, kUnspecified,
                                                 kUnspecified, kUnspecified);
    QCOMPARE(int(m.matrix), int(ColorMatrix::Bt709));
    QCOMPARE(int(m.range), int(ColorRange::Video));
    QCOMPARE(int(m.primaries), int(ColorPrimaries::Bt709));
    QCOMPARE(int(m.transfer), int(ColorTransfer::Bt709));
    QCOMPARE(int(m.chromaFormat), int(ChromaFormat::Yuv420));
    QCOMPARE(m.bitDepth, 8);
    QVERIFY(m == defaultColorMetadataForHeight(1080));
}

void TestColorMetadataPolicy::untaggedShortFrameIsBt601VideoNoOp() {
    const ColorMetadata m = resolveColorMetadata(VuiColorInfo{}, 480, kUnspecified, kUnspecified,
                                                 kUnspecified, kUnspecified);
    QCOMPARE(int(m.matrix), int(ColorMatrix::Bt601));
    QCOMPARE(int(m.range), int(ColorRange::Video));
    QCOMPARE(int(m.primaries), int(ColorPrimaries::Bt601));
    QCOMPARE(int(m.transfer), int(ColorTransfer::Bt601));
    QVERIFY(m == defaultColorMetadataForHeight(480));
}

void TestColorMetadataPolicy::vuiOverridesHeightDefault() {
    VuiColorInfo vui;
    vui.present = true;
    vui.range = ColorRange::Full;
    vui.primaries = ColorPrimaries::Bt709;
    vui.transfer = ColorTransfer::Bt709;
    vui.matrix = ColorMatrix::Bt709;

    const ColorMetadata m =
        resolveColorMetadata(vui, 480, kUnspecified, kUnspecified, kUnspecified, kUnspecified);
    QCOMPARE(int(m.matrix), int(ColorMatrix::Bt709));
    QCOMPARE(int(m.range), int(ColorRange::Full));
    QCOMPARE(int(m.primaries), int(ColorPrimaries::Bt709));
}

void TestColorMetadataPolicy::ffmpegCodesOverrideHeightDefaultWhenNoVui() {
    const ColorMetadata m = resolveColorMetadata(VuiColorInfo{}, 1080, 6, 1, 6, 6);
    QCOMPARE(int(m.matrix), int(ColorMatrix::Bt601));
    QCOMPARE(int(m.primaries), int(ColorPrimaries::Bt601));
    QCOMPARE(int(m.transfer), int(ColorTransfer::Bt601));
    QCOMPARE(int(m.range), int(ColorRange::Video));
}

QTEST_GUILESS_MAIN(TestColorMetadataPolicy)
#include "tst_colormetadatapolicy.moc"
