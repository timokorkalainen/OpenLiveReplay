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
    void differingSourceRatesAlignInWallTime();
    void sameRateZeroDriftIsExact();
    void nonzeroDriftIsBounded();
    void nonzeroDriftClaimWithoutAnchorSeparationIsBounded();
    void differingRatesAreBounded();
    void overflowingFrameConversionIsIncomparable();
    void overflowingConversionsThatCancelAreIncomparable();
    void int64MinIntermediateIsIncomparable();
    void overflowingSkewsThatCancelAreIncomparable();
    void overflowingOffsetIsIncomparable();
    void overflowingDriftBoundIsIncomparable();
    void resetClearsAnchors();
};

void TestTimecodeAlignerV2::aligned60pAnchorsTenSecondsApart() {
    TimecodeAlignerV2 aligner;
    aligner.observe(0, 0, {60, 1}, 0, {60, 1});
    aligner.observe(1, 600, {60, 1}, 600, {60, 1});
    const AlignmentOffset off = aligner.offset(0, 1);
    QVERIFY(off.comparable());
    QCOMPARE(off.offsetUs, int64_t(0));
}

void TestTimecodeAlignerV2::aligned5994AnchorsTenSecondsApart() {
    TimecodeAlignerV2 aligner;
    aligner.observe(0, 0, {60000, 1001}, 0, {60000, 1001});
    aligner.observe(1, 600, {60000, 1001}, 600, {60000, 1001});
    const AlignmentOffset off = aligner.offset(0, 1);
    QVERIFY(off.comparable());
    QCOMPARE(off.offsetUs, int64_t(0));
}

void TestTimecodeAlignerV2::lateSourceHasNegativeOffset() {
    TimecodeAlignerV2 aligner;
    aligner.observe(0, 0, {60, 1}, 0, {60, 1});
    aligner.observe(1, 600, {60, 1}, 603, {60, 1});
    const AlignmentOffset off = aligner.offset(0, 1);
    QVERIFY(off.comparable());
    QCOMPARE(off.offsetUs, int64_t(-50000));
}

void TestTimecodeAlignerV2::invalidOrMissingRateIsIncomparable() {
    TimecodeAlignerV2 aligner;
    aligner.observe(0, 0, {60, 1}, 0, {60, 1});
    aligner.observe(1, 0, {0, 0}, 0, {60, 1});
    QCOMPARE(aligner.offset(0, 1).kind, AlignmentOffset::Kind::Incomparable);
    QCOMPARE(aligner.offset(0, 2).kind, AlignmentOffset::Kind::Incomparable);
}

void TestTimecodeAlignerV2::differingSourceRatesAlignInWallTime() {
    TimecodeAlignerV2 aligner;
    aligner.observe(0, 500, {50, 1}, 600, {60, 1});
    aligner.observe(1, 600, {60, 1}, 600, {60, 1});
    const AlignmentOffset off = aligner.offset(0, 1);
    QVERIFY(off.comparable());
    QCOMPARE(off.offsetUs, int64_t(0));
}

void TestTimecodeAlignerV2::sameRateZeroDriftIsExact() {
    TimecodeAlignerV2 aligner;
    aligner.observe(0, 0, {60, 1}, 0, {60, 1});
    aligner.observe(1, 600, {60, 1}, 600, {60, 1});
    QCOMPARE(aligner.offset(0, 1).kind, AlignmentOffset::Kind::Exact);
}

void TestTimecodeAlignerV2::nonzeroDriftIsBounded() {
    TimecodeAlignerV2 aligner;
    aligner.observe(0, 0, {60, 1}, 0, {60, 1});
    aligner.observe(1, 600, {60, 1}, 600, {60, 1});
    QCOMPARE(aligner.offset(0, 1, 100).kind, AlignmentOffset::Kind::Bounded);
}

void TestTimecodeAlignerV2::nonzeroDriftClaimWithoutAnchorSeparationIsBounded() {
    TimecodeAlignerV2 aligner;
    aligner.observe(0, 0, {60, 1}, 0, {60, 1});
    aligner.observe(1, 0, {60, 1}, 0, {60, 1});
    QCOMPARE(aligner.offset(0, 1, 100).kind, AlignmentOffset::Kind::Bounded);
}

void TestTimecodeAlignerV2::differingRatesAreBounded() {
    TimecodeAlignerV2 aligner;
    aligner.observe(0, 500, {50, 1}, 600, {60, 1});
    aligner.observe(1, 600, {60, 1}, 600, {60, 1});
    QCOMPARE(aligner.offset(0, 1).kind, AlignmentOffset::Kind::Bounded);
}

void TestTimecodeAlignerV2::overflowingFrameConversionIsIncomparable() {
    TimecodeAlignerV2 aligner;
    aligner.observe(0, std::numeric_limits<int64_t>::max(), {60, 1}, 0, {60, 1});
    aligner.observe(1, 0, {60, 1}, 0, {60, 1});
    QCOMPARE(aligner.offset(0, 1).kind, AlignmentOffset::Kind::Incomparable);
}

void TestTimecodeAlignerV2::overflowingConversionsThatCancelAreIncomparable() {
    TimecodeAlignerV2 aligner;
    constexpr int64_t extreme = std::numeric_limits<int64_t>::max();
    aligner.observe(0, extreme, {60, 1}, extreme, {60, 1});
    aligner.observe(1, 0, {60, 1}, 0, {60, 1});
    QCOMPARE(aligner.offset(0, 1).kind, AlignmentOffset::Kind::Incomparable);
}

void TestTimecodeAlignerV2::int64MinIntermediateIsIncomparable() {
    TimecodeAlignerV2 aligner;
    constexpr int64_t kFramesProducingTwoToThe63Us = int64_t(1) << 62;
    aligner.observe(0, 0, {500000, 1}, 0, {500000, 1});
    aligner.observe(1, kFramesProducingTwoToThe63Us, {500000, 1}, 0, {500000, 1});
    QCOMPARE(aligner.offset(0, 1).kind, AlignmentOffset::Kind::Incomparable);
}

void TestTimecodeAlignerV2::overflowingSkewsThatCancelAreIncomparable() {
    TimecodeAlignerV2 aligner;
    constexpr FrameRateQ rate{1000000, 1};
    constexpr int64_t maximum = std::numeric_limits<int64_t>::max();
    constexpr int64_t minimum = std::numeric_limits<int64_t>::min();
    aligner.observe(0, maximum, rate, minimum, rate);
    aligner.observe(1, maximum, rate, minimum, rate);
    QCOMPARE(aligner.offset(0, 1).kind, AlignmentOffset::Kind::Incomparable);
}

void TestTimecodeAlignerV2::overflowingOffsetIsIncomparable() {
    TimecodeAlignerV2 aligner;
    constexpr FrameRateQ rate{1000000, 1};
    aligner.observe(0, 0, rate, std::numeric_limits<int64_t>::max(), rate);
    aligner.observe(1, 0, rate, std::numeric_limits<int64_t>::min(), rate);
    QCOMPARE(aligner.offset(0, 1).kind, AlignmentOffset::Kind::Incomparable);
}

void TestTimecodeAlignerV2::overflowingDriftBoundIsIncomparable() {
    TimecodeAlignerV2 aligner;
    constexpr FrameRateQ rate{1000000, 1};
    aligner.observe(0, 0, rate, 0, rate);
    aligner.observe(1, std::numeric_limits<int64_t>::max(), rate,
                    std::numeric_limits<int64_t>::max(), rate);
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
