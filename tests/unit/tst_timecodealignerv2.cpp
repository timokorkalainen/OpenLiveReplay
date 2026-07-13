#include <QtTest>

#include "recorder_engine/timing/timecodealignerv2.h"

#include <limits>

class TestTimecodeAlignerV2 : public QObject {
    Q_OBJECT
private slots:
    void aligned60pAnchorsTenSecondsApart();
    void aligned5994AnchorsTenSecondsApart();
    void lateSourceHasNegativeOffset();
    void invalidOrMissingRateIsIncomparable();
    void rejectsNegativeSessionFrame();
    void validatesObservationRates_data();
    void validatesObservationRates();
    void differingSourceRatesAlignInWallTime();
    void sameRateZeroDriftIsBoundedByArrivalQuantization();
    void nonzeroDriftIsBounded();
    void nonzeroDriftClaimWithoutAnchorSeparationIsBounded();
    void differingRatesAreBounded();
    void conversionBoundaryAcceptsLastRepresentable();
    void conversionBoundaryRejectsFirstOverflow();
    void offsetBoundaryAcceptsLastRepresentable();
    void offsetBoundaryRejectsFirstOverflow();
    void driftBoundaryAcceptsLastRepresentable();
    void driftBoundaryRejectsFirstOverflow();
    void resetClearsAnchors();
};

void TestTimecodeAlignerV2::aligned60pAnchorsTenSecondsApart() {
    TimecodeAlignerV2 aligner;
    aligner.observe(0, 0, {60, 1}, 0, {60, 1});
    aligner.observe(1, 600, {60, 1}, 600, {60, 1});
    const AlignmentOffset off = aligner.offset(0, 1);
    QVERIFY(off.comparable());
    QCOMPARE(off.kind, AlignmentOffset::Kind::Bounded);
    QCOMPARE(off.offsetUs, int64_t(0));
    QCOMPARE(off.boundUs, int64_t(16667));
}

void TestTimecodeAlignerV2::aligned5994AnchorsTenSecondsApart() {
    TimecodeAlignerV2 aligner;
    aligner.observe(0, 0, {60000, 1001}, 0, {60000, 1001});
    aligner.observe(1, 600, {60000, 1001}, 600, {60000, 1001});
    const AlignmentOffset off = aligner.offset(0, 1);
    QVERIFY(off.comparable());
    QCOMPARE(off.kind, AlignmentOffset::Kind::Bounded);
    QCOMPARE(off.offsetUs, int64_t(0));
    QCOMPARE(off.boundUs, int64_t(16684));
}

void TestTimecodeAlignerV2::lateSourceHasNegativeOffset() {
    TimecodeAlignerV2 aligner;
    aligner.observe(0, 0, {60, 1}, 0, {60, 1});
    aligner.observe(1, 600, {60, 1}, 603, {60, 1});
    const AlignmentOffset off = aligner.offset(0, 1);
    QVERIFY(off.comparable());
    QCOMPARE(off.kind, AlignmentOffset::Kind::Bounded);
    QCOMPARE(off.offsetUs, int64_t(-50000));
    QCOMPARE(off.boundUs, int64_t(16667));
}

void TestTimecodeAlignerV2::invalidOrMissingRateIsIncomparable() {
    TimecodeAlignerV2 aligner;
    aligner.observe(0, 0, {60, 1}, 0, {60, 1});
    aligner.observe(1, 0, {0, 0}, 0, {60, 1});
    QCOMPARE(aligner.offset(0, 1).kind, AlignmentOffset::Kind::Incomparable);
    QCOMPARE(aligner.offset(0, 2).kind, AlignmentOffset::Kind::Incomparable);
}

void TestTimecodeAlignerV2::rejectsNegativeSessionFrame() {
    TimecodeAlignerV2 aligner;
    aligner.observe(0, 0, {60, 1}, -1, {60, 1});
    QVERIFY(!aligner.hasTimecode(0));
    QCOMPARE(aligner.offset(0, 1).kind, AlignmentOffset::Kind::Incomparable);
}

void TestTimecodeAlignerV2::validatesObservationRates_data() {
    QTest::addColumn<int>("rateNum");
    QTest::addColumn<int>("rateDen");
    QTest::addColumn<bool>("accepted");

    QTest::newRow("zero") << 0 << 1 << false;
    QTest::newRow("below-lower-bound") << 1 << 1 << false;
    QTest::newRow("eleven-fps") << 11 << 1 << false;
    QTest::newRow("lower-bound") << 12 << 1 << true;
    QTest::newRow("supported-non-reduced") << 120 << 2 << true;
    QTest::newRow("upper-bound") << 240 << 1 << true;
    QTest::newRow("above-upper-bound") << 241 << 1 << false;
    QTest::newRow("extreme-high") << std::numeric_limits<int32_t>::max() << 1 << false;
    QTest::newRow("extreme-low") << 1 << std::numeric_limits<int32_t>::max() << false;
}

void TestTimecodeAlignerV2::validatesObservationRates() {
    QFETCH(int, rateNum);
    QFETCH(int, rateDen);
    QFETCH(bool, accepted);
    const FrameRateQ candidate{int32_t(rateNum), int32_t(rateDen)};

    TimecodeAlignerV2 aligner;
    aligner.observe(0, 0, candidate, 0, {60, 1});
    QCOMPARE(aligner.hasTimecode(0), accepted);

    aligner.reset();
    aligner.observe(0, 0, {60, 1}, 0, candidate);
    QCOMPARE(aligner.hasTimecode(0), accepted);
}

void TestTimecodeAlignerV2::differingSourceRatesAlignInWallTime() {
    TimecodeAlignerV2 aligner;
    aligner.observe(0, 500, {50, 1}, 600, {60, 1});
    aligner.observe(1, 600, {60, 1}, 600, {60, 1});
    const AlignmentOffset off = aligner.offset(0, 1);
    QVERIFY(off.comparable());
    QCOMPARE(off.kind, AlignmentOffset::Kind::Bounded);
    QCOMPARE(off.offsetUs, int64_t(0));
    QCOMPARE(off.boundUs, int64_t(16667));
}

void TestTimecodeAlignerV2::sameRateZeroDriftIsBoundedByArrivalQuantization() {
    TimecodeAlignerV2 aligner;
    aligner.observe(0, 0, {60, 1}, 0, {60, 1});
    aligner.observe(1, 600, {60, 1}, 600, {60, 1});
    const AlignmentOffset off = aligner.offset(0, 1);
    QCOMPARE(off.kind, AlignmentOffset::Kind::Bounded);
    QCOMPARE(off.boundUs, int64_t(16667));
}

void TestTimecodeAlignerV2::nonzeroDriftIsBounded() {
    TimecodeAlignerV2 aligner;
    aligner.observe(0, 0, {60, 1}, 0, {60, 1});
    aligner.observe(1, 600, {60, 1}, 600, {60, 1});
    const AlignmentOffset off = aligner.offset(0, 1, 100);
    QCOMPARE(off.kind, AlignmentOffset::Kind::Bounded);
    QCOMPARE(off.boundUs, int64_t(17667));
}

void TestTimecodeAlignerV2::nonzeroDriftClaimWithoutAnchorSeparationIsBounded() {
    TimecodeAlignerV2 aligner;
    aligner.observe(0, 0, {60, 1}, 0, {60, 1});
    aligner.observe(1, 0, {60, 1}, 0, {60, 1});
    const AlignmentOffset off = aligner.offset(0, 1, 100);
    QCOMPARE(off.kind, AlignmentOffset::Kind::Bounded);
    QCOMPARE(off.boundUs, int64_t(16667));
}

void TestTimecodeAlignerV2::differingRatesAreBounded() {
    TimecodeAlignerV2 aligner;
    aligner.observe(0, 500, {50, 1}, 600, {60, 1});
    aligner.observe(1, 600, {60, 1}, 600, {60, 1});
    const AlignmentOffset off = aligner.offset(0, 1);
    QCOMPARE(off.kind, AlignmentOffset::Kind::Bounded);
    QCOMPARE(off.boundUs, int64_t(16667));
}

void TestTimecodeAlignerV2::conversionBoundaryAcceptsLastRepresentable() {
    TimecodeAlignerV2 aligner;
    constexpr FrameRateQ rate{60000, 1001};
    constexpr int64_t lastConvertibleFrame = 552'849'472'738'548;
    aligner.observe(0, lastConvertibleFrame, rate, lastConvertibleFrame, rate);
    aligner.observe(1, 0, rate, 0, rate);
    const AlignmentOffset off = aligner.offset(0, 1);
    QCOMPARE(off.kind, AlignmentOffset::Kind::Bounded);
    QCOMPARE(off.offsetUs, int64_t(0));
    QCOMPARE(off.boundUs, int64_t(16684));
}

void TestTimecodeAlignerV2::conversionBoundaryRejectsFirstOverflow() {
    TimecodeAlignerV2 aligner;
    constexpr FrameRateQ rate{60000, 1001};
    constexpr int64_t firstOverflowingFrame = 552'849'472'738'549;
    aligner.observe(0, firstOverflowingFrame, rate, firstOverflowingFrame, rate);
    aligner.observe(1, 0, rate, 0, rate);
    QCOMPARE(aligner.offset(0, 1).kind, AlignmentOffset::Kind::Incomparable);
}

void TestTimecodeAlignerV2::offsetBoundaryAcceptsLastRepresentable() {
    TimecodeAlignerV2 aligner;
    constexpr FrameRateQ rate{60000, 1001};
    constexpr int64_t lastConvertibleFrame = 552'849'472'738'548;
    aligner.observe(0, 0, rate, lastConvertibleFrame, rate);
    aligner.observe(1, 0, rate, 0, rate);
    const AlignmentOffset off = aligner.offset(0, 1);
    QCOMPARE(off.kind, AlignmentOffset::Kind::Bounded);
    QCOMPARE(off.offsetUs, int64_t(9'223'372'036'854'775'800LL));
    QCOMPARE(off.boundUs, int64_t(16684));
}

void TestTimecodeAlignerV2::offsetBoundaryRejectsFirstOverflow() {
    TimecodeAlignerV2 aligner;
    constexpr FrameRateQ rate{60000, 1001};
    constexpr int64_t lastConvertibleFrame = 552'849'472'738'548;
    aligner.observe(0, 0, rate, lastConvertibleFrame, rate);
    aligner.observe(1, 1, rate, 0, rate);
    QCOMPARE(aligner.offset(0, 1).kind, AlignmentOffset::Kind::Incomparable);
}

void TestTimecodeAlignerV2::driftBoundaryAcceptsLastRepresentable() {
    TimecodeAlignerV2 aligner;
    constexpr FrameRateQ rate{60, 1};
    constexpr int64_t lastFrameWithRepresentableBound = 257'698'037'879;
    aligner.observe(0, 0, rate, 0, rate);
    aligner.observe(1, lastFrameWithRepresentableBound, rate, lastFrameWithRepresentableBound,
                    rate);
    const AlignmentOffset off = aligner.offset(0, 1, std::numeric_limits<int32_t>::max());
    QCOMPARE(off.kind, AlignmentOffset::Kind::Bounded);
    QCOMPARE(off.offsetUs, int64_t(0));
    QCOMPARE(off.boundUs, int64_t(9'223'372'036'819'000'363LL));
}

void TestTimecodeAlignerV2::driftBoundaryRejectsFirstOverflow() {
    TimecodeAlignerV2 aligner;
    constexpr FrameRateQ rate{60, 1};
    constexpr int64_t firstFrameWithOverflowingBound = 257'698'037'880;
    aligner.observe(0, 0, rate, 0, rate);
    aligner.observe(1, firstFrameWithOverflowingBound, rate, firstFrameWithOverflowingBound, rate);
    QCOMPARE(aligner.offset(0, 1, std::numeric_limits<int32_t>::max()).kind,
             AlignmentOffset::Kind::Incomparable);
}

void TestTimecodeAlignerV2::resetClearsAnchors() {
    TimecodeAlignerV2 aligner;
    aligner.observe(0, 0, {60, 1}, 0, {60, 1});
    aligner.observe(1, 0, {60, 1}, 0, {60, 1});
    QVERIFY(aligner.offset(0, 1).comparable());
    aligner.reset();
    QVERIFY(!aligner.hasTimecode(0));
    QCOMPARE(aligner.offset(0, 1).kind, AlignmentOffset::Kind::Incomparable);
}

QTEST_GUILESS_MAIN(TestTimecodeAlignerV2)
#include "tst_timecodealignerv2.moc"
