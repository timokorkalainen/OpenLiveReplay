#include <QtTest>

#include "recorder_engine/timing/timecodealignerv2.h"

#include <array>
#include <limits>

namespace {

struct CanonicalRateCase {
    const char* name = nullptr;
    FrameRateQ rate;
    bool dropFrame = false;
    int64_t framesPerDay = 0;
    int64_t roundedFrameUs = 0;
    int64_t fiftyPpmFrameBoundUs = 0;
};

constexpr std::array<CanonicalRateCase, 6> kCanonicalRates{{
    {"25", {25, 1}, false, 2'160'000, 40'000, 2},
    {"30000-1001", {30000, 1001}, true, 2'589'408, 33'367, 2},
    {"30", {30, 1}, false, 2'592'000, 33'333, 2},
    {"50", {50, 1}, false, 4'320'000, 20'000, 1},
    {"60000-1001", {60000, 1001}, true, 5'178'816, 16'683, 1},
    {"60", {60, 1}, false, 5'184'000, 16'667, 1},
}};

TimecodeEvidence evidence(int64_t frameOfDay, FrameRateQ labelRate, int64_t arrivalSessionFrame,
                          FrameRateQ sessionRate, bool dropFrame = false,
                          uint64_t sourceGeneration = 1, uint64_t timingGeneration = 1,
                          int64_t quantizationBoundUs = 0, int64_t driftBoundUs = 0,
                          bool discontinuity = false) {
    TimecodeEvidence value;
    value.frameOfDay = frameOfDay;
    value.labelRate = labelRate;
    value.sourceGeneration = sourceGeneration;
    value.timingGeneration = timingGeneration;
    value.provenance = TimecodeProvenance::H264PicTiming;
    value.dropFrame = dropFrame;
    value.discontinuity = discontinuity;
    value.arrivalSessionFrame = arrivalSessionFrame;
    value.sessionRate = sessionRate;
    value.quantizationBoundUs = quantizationBoundUs;
    value.driftBoundUs = driftBoundUs;
    return value;
}

const CanonicalRateCase& canonicalRate(const QByteArray& name) {
    for (const CanonicalRateCase& candidate : kCanonicalRates) {
        if (name == candidate.name) return candidate;
    }
    Q_UNREACHABLE();
}

} // namespace

class TestTimecodeAlignerV2 : public QObject {
    Q_OBJECT
private slots:
    void rolloverGrid_data();
    void rolloverGrid();
    void proofGridMatchesExactOracle_data();
    void proofGridMatchesExactOracle();
    void mixedRatesAlignInWallTime();
    void ambiguousMultiDayGapInvalidatesSource();
    void explicitDiscontinuityInvalidatesGeneration();
    void newerGenerationsReanchorAndStaleEvidenceCannotRestore();
    void lateObservationDoesNotReplaceCurrentState();
    void pairBoundSumsAllEvidenceUncertainty();
    void negativeEvidenceNeverAnchors_data();
    void negativeEvidenceNeverAnchors();
    void checkedOverflowIsIncomparable();
    void resetSourceClearsAnchorGenerationAndUnwrapState();
    void resetClearsEverySource();
};

void TestTimecodeAlignerV2::rolloverGrid_data() {
    QTest::addColumn<QByteArray>("rateName");
    for (const CanonicalRateCase& rate : kCanonicalRates)
        QTest::newRow(rate.name) << QByteArray(rate.name);
}

void TestTimecodeAlignerV2::rolloverGrid() {
    QFETCH(QByteArray, rateName);
    const CanonicalRateCase& row = canonicalRate(rateName);
    const int64_t lastFrame = row.framesPerDay - 1;

    TimecodeAlignerV2 aligner;
    aligner.observe(0, evidence(lastFrame, row.rate, lastFrame, row.rate, row.dropFrame));
    aligner.observe(0, evidence(0, row.rate, row.framesPerDay, row.rate, row.dropFrame));
    aligner.observe(1, evidence(0, row.rate, row.framesPerDay, row.rate, row.dropFrame, 2));

    const AlignmentOffset offset = aligner.offset(0, 1);
    QCOMPARE(offset.kind, AlignmentOffset::Kind::Exact);
    QCOMPARE(offset.offsetUs, int64_t(0));
    QCOMPARE(offset.boundUs, int64_t(0));
    QCOMPARE(offset.sourceGenerationA, uint64_t(1));
    QCOMPARE(offset.sourceGenerationB, uint64_t(2));
}

void TestTimecodeAlignerV2::proofGridMatchesExactOracle_data() {
    QTest::addColumn<QByteArray>("rateName");
    for (const CanonicalRateCase& rate : kCanonicalRates)
        QTest::newRow(rate.name) << QByteArray(rate.name);
}

void TestTimecodeAlignerV2::proofGridMatchesExactOracle() {
    QFETCH(QByteArray, rateName);
    const CanonicalRateCase& row = canonicalRate(rateName);
    const int64_t lastFrame = row.framesPerDay - 1;

    TimecodeAlignerV2 aligner;
    aligner.observe(
        0, evidence(lastFrame, row.rate, lastFrame, row.rate, row.dropFrame, 10, 20, 3, 7));
    aligner.observe(
        1, evidence(0, row.rate, row.framesPerDay + 1, row.rate, row.dropFrame, 11, 21, 5, 11));

    const AlignmentOffset offset = aligner.offset(0, 1, 50);
    QCOMPARE(offset.kind, AlignmentOffset::Kind::Bounded);
    QCOMPARE(offset.offsetUs, -row.roundedFrameUs);
    QCOMPARE(offset.boundUs, int64_t(26 + row.fiftyPpmFrameBoundUs));
    QCOMPARE(offset.sourceGenerationA, uint64_t(10));
    QCOMPARE(offset.sourceGenerationB, uint64_t(11));
}

void TestTimecodeAlignerV2::mixedRatesAlignInWallTime() {
    TimecodeAlignerV2 aligner;
    aligner.observe(0, evidence(25, {25, 1}, 60, {60, 1}));
    aligner.observe(1, evidence(60, {60, 1}, 60, {60, 1}, false, 2));

    const AlignmentOffset offset = aligner.offset(0, 1);
    QCOMPARE(offset.kind, AlignmentOffset::Kind::Exact);
    QCOMPARE(offset.offsetUs, int64_t(0));
}

void TestTimecodeAlignerV2::ambiguousMultiDayGapInvalidatesSource() {
    constexpr CanonicalRateCase row = kCanonicalRates[5];
    TimecodeAlignerV2 aligner;
    aligner.observe(0, evidence(120, row.rate, 120, row.rate));
    aligner.observe(1, evidence(120, row.rate, 120, row.rate, false, 2));
    QVERIFY(aligner.offset(0, 1).comparable());

    aligner.observe(1, evidence(120, row.rate, 120 + 2 * row.framesPerDay, row.rate, false, 2));
    QVERIFY(!aligner.hasTimecode(1));
    QCOMPARE(aligner.offset(0, 1).kind, AlignmentOffset::Kind::Incomparable);
}

void TestTimecodeAlignerV2::explicitDiscontinuityInvalidatesGeneration() {
    TimecodeAlignerV2 aligner;
    aligner.observe(0, evidence(0, {60, 1}, 0, {60, 1}));
    aligner.observe(1, evidence(0, {60, 1}, 0, {60, 1}, false, 2));
    QVERIFY(aligner.offset(0, 1).comparable());

    aligner.observe(1, evidence(1, {60, 1}, 1, {60, 1}, false, 2, 1, 0, 0, true));
    QVERIFY(!aligner.hasTimecode(1));
    QCOMPARE(aligner.offset(0, 1).kind, AlignmentOffset::Kind::Incomparable);

    aligner.observe(1, evidence(2, {60, 1}, 2, {60, 1}, false, 2, 1));
    QVERIFY(!aligner.hasTimecode(1));
    aligner.observe(1, evidence(2, {60, 1}, 2, {60, 1}, false, 2, 2));
    QVERIFY(aligner.hasTimecode(1));
}

void TestTimecodeAlignerV2::newerGenerationsReanchorAndStaleEvidenceCannotRestore() {
    TimecodeAlignerV2 aligner;
    aligner.observe(0, evidence(0, {60, 1}, 0, {60, 1}, false, 10));
    aligner.observe(1, evidence(0, {60, 1}, 0, {60, 1}, false, 20));

    aligner.observe(1, evidence(60, {60, 1}, 60, {60, 1}, false, 21));
    AlignmentOffset offset = aligner.offset(0, 1);
    QCOMPARE(offset.kind, AlignmentOffset::Kind::Exact);
    QCOMPARE(offset.offsetUs, int64_t(0));
    QCOMPARE(offset.sourceGenerationB, uint64_t(21));

    aligner.observe(1, evidence(0, {60, 1}, 120, {60, 1}, false, 20));
    offset = aligner.offset(0, 1);
    QCOMPARE(offset.offsetUs, int64_t(0));
    QCOMPARE(offset.sourceGenerationB, uint64_t(21));

    aligner.observe(1, evidence(60, {60, 1}, 63, {60, 1}, false, 21, 2));
    offset = aligner.offset(0, 1);
    QCOMPARE(offset.offsetUs, int64_t(-50'000));
    aligner.observe(1, evidence(60, {60, 1}, 120, {60, 1}, false, 21, 1));
    QCOMPARE(aligner.offset(0, 1).offsetUs, int64_t(-50'000));
}

void TestTimecodeAlignerV2::lateObservationDoesNotReplaceCurrentState() {
    TimecodeAlignerV2 aligner;
    aligner.observe(0, evidence(100, {60, 1}, 100, {60, 1}));
    aligner.observe(1, evidence(100, {60, 1}, 100, {60, 1}, false, 2));
    aligner.observe(1, evidence(101, {60, 1}, 101, {60, 1}, false, 2));
    aligner.observe(1, evidence(99, {60, 1}, 99, {60, 1}, false, 2));

    const AlignmentOffset offset = aligner.offset(0, 1);
    QCOMPARE(offset.kind, AlignmentOffset::Kind::Exact);
    QCOMPARE(offset.offsetUs, int64_t(0));
}

void TestTimecodeAlignerV2::pairBoundSumsAllEvidenceUncertainty() {
    TimecodeAlignerV2 aligner;
    aligner.observe(0, evidence(0, {60, 1}, 0, {60, 1}, false, 1, 1, 3, 7));
    aligner.observe(1, evidence(0, {60, 1}, 0, {60, 1}, false, 2, 1, 5, 11));

    const AlignmentOffset offset = aligner.offset(0, 1);
    QCOMPARE(offset.kind, AlignmentOffset::Kind::Bounded);
    QCOMPARE(offset.offsetUs, int64_t(0));
    QCOMPARE(offset.boundUs, int64_t(26));
}

void TestTimecodeAlignerV2::negativeEvidenceNeverAnchors_data() {
    QTest::addColumn<int>("field");
    QTest::newRow("negative-frame-of-day") << 0;
    QTest::newRow("negative-arrival") << 1;
    QTest::newRow("negative-quantization") << 2;
    QTest::newRow("negative-drift") << 3;
}

void TestTimecodeAlignerV2::negativeEvidenceNeverAnchors() {
    QFETCH(int, field);
    TimecodeEvidence invalid = evidence(0, {60, 1}, 0, {60, 1});
    if (field == 0) invalid.frameOfDay = -1;
    if (field == 1) invalid.arrivalSessionFrame = -1;
    if (field == 2) invalid.quantizationBoundUs = -1;
    if (field == 3) invalid.driftBoundUs = -1;

    TimecodeAlignerV2 aligner;
    aligner.observe(0, invalid);
    QVERIFY(!aligner.hasTimecode(0));
}

void TestTimecodeAlignerV2::checkedOverflowIsIncomparable() {
    TimecodeAlignerV2 conversionOverflow;
    conversionOverflow.observe(0,
                               evidence(0, {12, 1}, std::numeric_limits<int64_t>::max(), {12, 1}));
    conversionOverflow.observe(1, evidence(0, {12, 1}, 0, {12, 1}, false, 2));
    QCOMPARE(conversionOverflow.offset(0, 1).kind, AlignmentOffset::Kind::Incomparable);

    TimecodeAlignerV2 boundOverflow;
    boundOverflow.observe(
        0, evidence(0, {60, 1}, 0, {60, 1}, false, 1, 1, std::numeric_limits<int64_t>::max()));
    boundOverflow.observe(1, evidence(0, {60, 1}, 0, {60, 1}, false, 2, 1, 1));
    QCOMPARE(boundOverflow.offset(0, 1).kind, AlignmentOffset::Kind::Incomparable);
}

void TestTimecodeAlignerV2::resetSourceClearsAnchorGenerationAndUnwrapState() {
    constexpr CanonicalRateCase row = kCanonicalRates[5];
    TimecodeAlignerV2 aligner;
    aligner.observe(
        0, evidence(row.framesPerDay - 1, row.rate, row.framesPerDay - 1, row.rate, false, 9));
    aligner.observe(0, evidence(0, row.rate, row.framesPerDay, row.rate, false, 9));
    aligner.resetSource(0);
    QVERIFY(!aligner.hasTimecode(0));

    aligner.observe(0, evidence(0, row.rate, 0, row.rate, false, 9));
    aligner.observe(1, evidence(0, row.rate, 0, row.rate, false, 10));
    const AlignmentOffset offset = aligner.offset(0, 1);
    QCOMPARE(offset.kind, AlignmentOffset::Kind::Exact);
    QCOMPARE(offset.offsetUs, int64_t(0));
}

void TestTimecodeAlignerV2::resetClearsEverySource() {
    TimecodeAlignerV2 aligner;
    aligner.observe(0, evidence(0, {60, 1}, 0, {60, 1}));
    aligner.observe(1, evidence(0, {60, 1}, 0, {60, 1}, false, 2));
    QVERIFY(aligner.offset(0, 1).comparable());
    aligner.reset();
    QVERIFY(!aligner.hasTimecode(0));
    QCOMPARE(aligner.offset(0, 1).kind, AlignmentOffset::Kind::Incomparable);
}

QTEST_GUILESS_MAIN(TestTimecodeAlignerV2)
#include "tst_timecodealignerv2.moc"
