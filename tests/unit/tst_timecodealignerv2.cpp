#include <QtTest>

#include "recorder_engine/timing/timecodealignerv2.h"

class TestTimecodeAlignerV2 : public QObject {
    Q_OBJECT
private slots:
    void aligned60pAnchorsTenSecondsApart();
    void aligned5994AnchorsTenSecondsApart();
    void lateSourceHasNegativeOffset();
    void invalidOrMissingRateIsIncomparable();
    void differingSourceRatesAlignInWallTime();
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
