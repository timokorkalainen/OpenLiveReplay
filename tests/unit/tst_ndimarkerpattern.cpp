#include <QtTest>

#include "tests/e2e/ndi_marker_pattern.h"

#include <algorithm>
#include <cmath>

class TestNdiMarkerPattern : public QObject {
    Q_OBJECT

private slots:
    void videoFlashesForTheFirstTwoFramesOfEachSecond();
    void audioBeepsOnlyDuringTheFlashWindow();
    void timestampsApplyMediaSkewIn100nsUnits();
    void timecodeParsesStartAndAdvancesWithFrames();
    void staticTimecodeHoldsStartOnEveryFrame();
    void rationalRateFlashesOnSourceSecondBoundary();
    void rationalRateAudioDistributesSamplesAcrossFrames();
};

void TestNdiMarkerPattern::videoFlashesForTheFirstTwoFramesOfEachSecond() {
    NdiMarkerConfig config;
    config.width = 4;
    config.height = 2;

    std::vector<uint8_t> pixels;
    fillNdiMarkerUyvyFrame(config, 0, pixels);
    QCOMPARE(pixels.size(), size_t(config.width * config.height * 2));
    QCOMPARE(int(pixels[1]), 235);
    QCOMPARE(int(pixels[3]), 235);

    fillNdiMarkerUyvyFrame(config, 1, pixels);
    QCOMPARE(int(pixels[1]), 235);

    fillNdiMarkerUyvyFrame(config, 2, pixels);
    QCOMPARE(int(pixels[1]), 16);
    QCOMPARE(int(pixels[3]), 16);

    fillNdiMarkerUyvyFrame(config, 30, pixels);
    QCOMPARE(int(pixels[1]), 235);
}

void TestNdiMarkerPattern::audioBeepsOnlyDuringTheFlashWindow() {
    NdiMarkerConfig config;
    std::vector<float> audio;

    fillNdiMarkerAudioFrame(config, 0, audio);
    QCOMPARE(audio.size(), size_t(config.channels * ndiMarkerSamplesPerFrame(config)));
    const auto maxActive = std::max_element(
        audio.begin(), audio.end(), [](float a, float b) { return std::abs(a) < std::abs(b); });
    QVERIFY(maxActive != audio.end());
    QVERIFY(std::abs(*maxActive) > 0.1f);

    fillNdiMarkerAudioFrame(config, 2, audio);
    const auto maxSilent = std::max_element(
        audio.begin(), audio.end(), [](float a, float b) { return std::abs(a) < std::abs(b); });
    QVERIFY(maxSilent != audio.end());
    QCOMPARE(*maxSilent, 0.0f);
}

void TestNdiMarkerPattern::timestampsApplyMediaSkewIn100nsUnits() {
    NdiMarkerConfig config;
    config.skewPpm = 200.0;

    QCOMPARE(ndiMarkerTimestamp100ns(config, 0), int64_t(0));
    QCOMPARE(ndiMarkerTimestamp100ns(config, 300), int64_t(100020000));
}

void TestNdiMarkerPattern::timecodeParsesStartAndAdvancesWithFrames() {
    NdiMarkerConfig config;
    config.startTimecode = QStringLiteral("10:00:00:00");

    QCOMPARE(ndiMarkerStartTimecode100ns(config), int64_t(360000000000));
    QCOMPARE(ndiMarkerTimecode100ns(config, 30), int64_t(360010000000));
}

void TestNdiMarkerPattern::staticTimecodeHoldsStartOnEveryFrame() {
    NdiMarkerConfig config;
    config.startTimecode = QStringLiteral("10:00:00:00");
    config.staticTimecode = true;

    // Every frame carries the SAME injected TC (no per-frame advance), so the
    // engine's first muxed frame — captured at an arbitrary connect time — records
    // it frame-exact. 10:00:00:00 == 36000 s == 360000000000 (100 ns).
    QCOMPARE(ndiMarkerTimecode100ns(config, 0), int64_t(360000000000));
    QCOMPARE(ndiMarkerTimecode100ns(config, 30), int64_t(360000000000));
    QCOMPARE(ndiMarkerTimecode100ns(config, 599), int64_t(360000000000));
}

void TestNdiMarkerPattern::rationalRateFlashesOnSourceSecondBoundary() {
    NdiMarkerConfig config;
    config.frameRateNumerator = 30000;
    config.frameRateDenominator = 1001;

    QVERIFY(ndiMarkerIsActive(config, 0));
    QVERIFY(ndiMarkerIsActive(config, 1));
    QVERIFY(!ndiMarkerIsActive(config, 2));
    QVERIFY(!ndiMarkerIsActive(config, 29));
    QVERIFY(ndiMarkerIsActive(config, 30));
}

void TestNdiMarkerPattern::rationalRateAudioDistributesSamplesAcrossFrames() {
    NdiMarkerConfig config;
    config.frameRateNumerator = 30000;
    config.frameRateDenominator = 1001;

    QCOMPARE(ndiMarkerSamplesForFrame(config, 0), 1602);
    QCOMPARE(ndiMarkerSamplesForFrame(config, 1), 1601);

    int total = 0;
    for (int frame = 0; frame < 30; ++frame) {
        total += ndiMarkerSamplesForFrame(config, frame);
    }
    QCOMPARE(total, 48048);

    std::vector<float> audio;
    fillNdiMarkerAudioFrame(config, 1, audio);
    QCOMPARE(audio.size(), size_t(config.channels * 1601));
}

QTEST_GUILESS_MAIN(TestNdiMarkerPattern)
#include "tst_ndimarkerpattern.moc"
