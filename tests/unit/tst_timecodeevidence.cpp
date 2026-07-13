#include <QtTest>

#include "recorder_engine/timing/timecodeevidence.h"

#include <cmath>
#include <cstdint>
#include <limits>

class TestTimecodeEvidence : public QObject {
    Q_OBJECT

private slots:
    void validatesCanonicalLabelRates_data();
    void validatesCanonicalLabelRates();
    void dropFrameRejectsSkippedLabels();
    void rejectsDropFrameAtNonDropRates();
    void rejectsInvalidAndExtremeRationals();
    void rejectsInvalidEvidenceValues();
    void canonicalizesStandardRates_data();
    void canonicalizesStandardRates();
    void canonicalizesNtscRatesExactly();
    void rationalizesOtherSupportedRates();
    void rejectsUnsupportedMetadataRates();
};

void TestTimecodeEvidence::validatesCanonicalLabelRates_data() {
    QTest::addColumn<int>("rateNum");
    QTest::addColumn<int>("rateDen");
    QTest::addColumn<int>("nominalLabelRate");

    QTest::newRow("25") << 25 << 1 << 25;
    QTest::newRow("30000/1001") << 30000 << 1001 << 30;
    QTest::newRow("30") << 30 << 1 << 30;
    QTest::newRow("50") << 50 << 1 << 50;
    QTest::newRow("60000/1001") << 60000 << 1001 << 60;
    QTest::newRow("60") << 60 << 1 << 60;
}

void TestTimecodeEvidence::validatesCanonicalLabelRates() {
    QFETCH(int, rateNum);
    QFETCH(int, rateDen);
    QFETCH(int, nominalLabelRate);
    const FrameRateQ rate{int32_t(rateNum), int32_t(rateDen)};

    QVERIFY(validateTimecodeLabel({23, 59, 59, nominalLabelRate - 1, false, true}, rate));
    QVERIFY(!validateTimecodeLabel({23, 59, 59, nominalLabelRate, false, true}, rate));
}

void TestTimecodeEvidence::dropFrameRejectsSkippedLabels() {
    const FrameRateQ rate{30000, 1001};
    QVERIFY(!validateTimecodeLabel({0, 1, 0, 0, true, true}, rate));
    QVERIFY(!validateTimecodeLabel({0, 1, 0, 1, true, true}, rate));
    QVERIFY(validateTimecodeLabel({0, 1, 0, 2, true, true}, rate));
}

void TestTimecodeEvidence::rejectsDropFrameAtNonDropRates() {
    QVERIFY(!validateTimecodeLabel({0, 1, 0, 0, true, true}, {30, 1}));
    QVERIFY(!validateTimecodeLabel({0, 1, 0, 0, true, true}, {25, 1}));
    QVERIFY(validateTimecodeLabel({0, 10, 0, 0, true, true}, {30000, 1001}));
    QVERIFY(!validateTimecodeLabel({0, 1, 0, 3, true, true}, {60000, 1001}));
    QVERIFY(validateTimecodeLabel({0, 1, 0, 4, true, true}, {60000, 1001}));
}

void TestTimecodeEvidence::rejectsInvalidAndExtremeRationals() {
    const Smpte12mTimecode label{0, 0, 0, 0, false, true};
    QVERIFY(!validateTimecodeLabel(label, {0, 1}));
    QVERIFY(!validateTimecodeLabel(label, {25, 0}));
    QVERIFY(!validateTimecodeLabel(label, {-25, 1}));
    QVERIFY(!validateTimecodeLabel(label, {25, -1}));
    QVERIFY(!validateTimecodeLabel(label, {std::numeric_limits<int32_t>::max(), 1}));
    QVERIFY(!validateTimecodeLabel(label, {1, std::numeric_limits<int32_t>::max()}));
}

void TestTimecodeEvidence::rejectsInvalidEvidenceValues() {
    TimecodeEvidence evidence;
    evidence.frameOfDay = 0;
    evidence.labelRate = {25, 1};
    evidence.arrivalSessionFrame = 0;
    evidence.sessionRate = {25, 1};
    QVERIFY(evidence.valid());

    evidence.frameOfDay = std::numeric_limits<int64_t>::min();
    QVERIFY(!evidence.valid());
    evidence.frameOfDay = 0;
    evidence.quantizationBoundUs = std::numeric_limits<int64_t>::min();
    QVERIFY(!evidence.valid());
    evidence.quantizationBoundUs = 0;
    evidence.driftBoundUs = std::numeric_limits<int64_t>::min();
    QVERIFY(!evidence.valid());
}

void TestTimecodeEvidence::canonicalizesStandardRates_data() {
    QTest::addColumn<double>("fps");
    QTest::addColumn<int>("rateNum");
    QTest::addColumn<int>("rateDen");

    QTest::newRow("25") << 25.0 << 25 << 1;
    QTest::newRow("30000/1001") << (30000.0 / 1001.0) << 30000 << 1001;
    QTest::newRow("30") << 30.0 << 30 << 1;
    QTest::newRow("50") << 50.0 << 50 << 1;
    QTest::newRow("60000/1001") << (60000.0 / 1001.0) << 60000 << 1001;
    QTest::newRow("60") << 60.0 << 60 << 1;
}

void TestTimecodeEvidence::canonicalizesStandardRates() {
    QFETCH(double, fps);
    QFETCH(int, rateNum);
    QFETCH(int, rateDen);
    const auto rate = canonicalFrameRate(fps);
    QVERIFY(rate.has_value());
    QCOMPARE(rate->num, int32_t(rateNum));
    QCOMPARE(rate->den, int32_t(rateDen));
}

void TestTimecodeEvidence::canonicalizesNtscRatesExactly() {
    const auto rate5994 = canonicalFrameRate(59.94);
    const auto rate2997 = canonicalFrameRate(29.97);
    QVERIFY(rate5994.has_value());
    QVERIFY(rate2997.has_value());
    QCOMPARE(rate5994->num, int32_t(60000));
    QCOMPARE(rate5994->den, int32_t(1001));
    QCOMPARE(rate2997->num, int32_t(30000));
    QCOMPARE(rate2997->den, int32_t(1001));
}

void TestTimecodeEvidence::rationalizesOtherSupportedRates() {
    const auto rate = canonicalFrameRate(23.976);
    QVERIFY(rate.has_value());
    QVERIFY(rate->valid());
    QVERIFY(rate->den <= 100000);
    QVERIFY(std::abs(double(rate->num) / rate->den - 23.976) < 1e-9);
}

void TestTimecodeEvidence::rejectsUnsupportedMetadataRates() {
    QVERIFY(!canonicalFrameRate(0.0).has_value());
    QVERIFY(!canonicalFrameRate(-25.0).has_value());
    QVERIFY(!canonicalFrameRate(11.999).has_value());
    QVERIFY(!canonicalFrameRate(240.001).has_value());
    QVERIFY(!canonicalFrameRate(std::numeric_limits<double>::infinity()).has_value());
    QVERIFY(!canonicalFrameRate(-std::numeric_limits<double>::infinity()).has_value());
    QVERIFY(!canonicalFrameRate(std::numeric_limits<double>::quiet_NaN()).has_value());
    QVERIFY(!canonicalFrameRate(std::numeric_limits<double>::max()).has_value());
}

QTEST_GUILESS_MAIN(TestTimecodeEvidence)
#include "tst_timecodeevidence.moc"
