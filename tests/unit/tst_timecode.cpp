#include <QtTest>

#include "recorder_engine/timing/timecode.h"

#include <string>

namespace Timecode = olr::Timecode;
using olr::TimecodeRate;

namespace {
// Canonical rate constructors used throughout the vectors.
constexpr TimecodeRate kRate30 = {30, 1};         // NDF 30
constexpr TimecodeRate kRate2997 = {30000, 1001}; // DF 29.97
constexpr TimecodeRate kRate5994 = {60000, 1001}; // DF 59.94
constexpr TimecodeRate kRate25 = {25, 1};         // NDF 25
constexpr TimecodeRate kRate60 = {60, 1};         // NDF 60 (invalid for DF)
} // namespace

class TestTimecode : public QObject {
    Q_OBJECT

private slots:
    // --- NDF 30 -----------------------------------------------------------
    void ndf30_basicVectors();
    void ndf30_dropFrameFlagIgnoredForIntegerRate();

    // --- DF 29.97 ---------------------------------------------------------
    void df2997_zero();
    void df2997_minuteOneSkipsZeroAndOne();
    void df2997_minuteTenNoDrop();
    void df2997_oneHourVector();

    // --- DF 59.94 ---------------------------------------------------------
    void df5994_minuteOneSkipsFour();
    void df5994_minuteTenNoDrop();
    void df5994_oneHourVector();

    // --- Round trips ------------------------------------------------------
    void roundTripNdf30();
    void roundTripDf2997();
    void roundTripDf5994();

    // --- Invalid DF contract ----------------------------------------------
    void dropFrameRequestedOnInvalidRateFallsBackToNdf();
    void separatorReflectsEffectiveMode();

    // --- Parsing edge cases -----------------------------------------------
    void parseAcceptsBothSeparators();
    void parseRejectsMalformedInput();
    void parseOversizedFieldDoesNotOverflow();
};

void TestTimecode::ndf30_basicVectors() {
    QCOMPARE(Timecode::framesToTimecode(0, kRate30, false), std::string("00:00:00:00"));
    QCOMPARE(Timecode::framesToTimecode(30, kRate30, false), std::string("00:00:01:00"));
    QCOMPARE(Timecode::framesToTimecode(1800, kRate30, false), std::string("00:01:00:00"));
    // One full hour at 30 fps NDF.
    QCOMPARE(Timecode::framesToTimecode(108000, kRate30, false), std::string("01:00:00:00"));
}

void TestTimecode::ndf30_dropFrameFlagIgnoredForIntegerRate() {
    // 30/1 is not a valid drop-frame rate: DF request must fall back to NDF,
    // including the colon separator.
    QCOMPARE(Timecode::framesToTimecode(1800, kRate30, true), std::string("00:01:00:00"));
}

void TestTimecode::df2997_zero() {
    QCOMPARE(Timecode::framesToTimecode(0, kRate2997, true), std::string("00:00:00;00"));
}

void TestTimecode::df2997_minuteOneSkipsZeroAndOne() {
    // Frame 1799 -> 00:00:59;29, frame 1800 -> 00:01:00;02 (00 and 01 dropped).
    QCOMPARE(Timecode::framesToTimecode(1799, kRate2997, true), std::string("00:00:59;29"));
    QCOMPARE(Timecode::framesToTimecode(1800, kRate2997, true), std::string("00:01:00;02"));
}

void TestTimecode::df2997_minuteTenNoDrop() {
    // At minute 10 there is NO drop: 00:09:59;29 -> 00:10:00;00.
    const int64_t f959 = Timecode::timecodeToFrames("00:09:59;29", kRate2997, true);
    const int64_t f1000 = Timecode::timecodeToFrames("00:10:00;00", kRate2997, true);
    QCOMPARE(f1000, f959 + 1);
    QCOMPARE(Timecode::framesToTimecode(f959, kRate2997, true), std::string("00:09:59;29"));
    QCOMPARE(Timecode::framesToTimecode(f1000, kRate2997, true), std::string("00:10:00;00"));
}

void TestTimecode::df2997_oneHourVector() {
    // Canonical SMPTE DF 29.97: frame 107892 == 01:00:00;00.
    QCOMPARE(Timecode::framesToTimecode(107892, kRate2997, true), std::string("01:00:00;00"));
    QCOMPARE(Timecode::timecodeToFrames("01:00:00;00", kRate2997, true), int64_t(107892));
}

void TestTimecode::df5994_minuteOneSkipsFour() {
    // 59.94 drops 0,1,2,3 at the start of each minute (except every 10th).
    QCOMPARE(Timecode::framesToTimecode(3599, kRate5994, true), std::string("00:00:59;59"));
    QCOMPARE(Timecode::framesToTimecode(3600, kRate5994, true), std::string("00:01:00;04"));
}

void TestTimecode::df5994_minuteTenNoDrop() {
    const int64_t f959 = Timecode::timecodeToFrames("00:09:59;59", kRate5994, true);
    const int64_t f1000 = Timecode::timecodeToFrames("00:10:00;00", kRate5994, true);
    QCOMPARE(f1000, f959 + 1);
    QCOMPARE(Timecode::framesToTimecode(f1000, kRate5994, true), std::string("00:10:00;00"));
}

void TestTimecode::df5994_oneHourVector() {
    // One hour at 59.94 DF is exactly twice the 29.97 hour count.
    QCOMPARE(Timecode::framesToTimecode(215784, kRate5994, true), std::string("01:00:00;00"));
    QCOMPARE(Timecode::timecodeToFrames("01:00:00;00", kRate5994, true), int64_t(215784));
}

void TestTimecode::roundTripNdf30() {
    int checked = 0;
    for (int64_t n = 0; n < 130000; n += 7) {
        const std::string tc = Timecode::framesToTimecode(n, kRate30, false);
        QCOMPARE(Timecode::timecodeToFrames(tc, kRate30, false), n);
        ++checked;
    }
    QVERIFY(checked > 18000);
}

void TestTimecode::roundTripDf2997() {
    int checked = 0;
    for (int64_t n = 0; n < 130000; n += 7) {
        const std::string tc = Timecode::framesToTimecode(n, kRate2997, true);
        QCOMPARE(Timecode::timecodeToFrames(tc, kRate2997, true), n);
        ++checked;
    }
    QVERIFY(checked > 18000);
}

void TestTimecode::roundTripDf5994() {
    int checked = 0;
    for (int64_t n = 0; n < 260000; n += 11) {
        const std::string tc = Timecode::framesToTimecode(n, kRate5994, true);
        QCOMPARE(Timecode::timecodeToFrames(tc, kRate5994, true), n);
        ++checked;
    }
    QVERIFY(checked > 20000);
}

void TestTimecode::dropFrameRequestedOnInvalidRateFallsBackToNdf() {
    // Contract: DF requested on a non-DF rate (25, 60, 30) silently falls back
    // to NDF. isDropFrameRate() reports the truth so callers can detect it.
    QVERIFY(!Timecode::isDropFrameRate(kRate25));
    QVERIFY(!Timecode::isDropFrameRate(kRate60));
    QVERIFY(!Timecode::isDropFrameRate(kRate30));
    QVERIFY(Timecode::isDropFrameRate(kRate2997));
    QVERIFY(Timecode::isDropFrameRate(kRate5994));

    // 25 fps NDF: frame 25 -> 00:00:01:00. DF flag must be ignored.
    QCOMPARE(Timecode::framesToTimecode(25, kRate25, true), std::string("00:00:01:00"));
    // 60 fps NDF with DF requested: behaves exactly like NDF (colon separator).
    QCOMPARE(Timecode::framesToTimecode(60, kRate60, true), std::string("00:00:01:00"));
}

void TestTimecode::separatorReflectsEffectiveMode() {
    // NDF always uses a colon before the frame field; DF uses a semicolon.
    const std::string ndf = Timecode::framesToTimecode(1800, kRate30, false);
    QVERIFY(ndf.find(';') == std::string::npos);
    const std::string df = Timecode::framesToTimecode(1800, kRate2997, true);
    QVERIFY(df.find(';') != std::string::npos);
    // DF flag false on a DF-capable rate yields NDF formatting (colon).
    const std::string forcedNdf = Timecode::framesToTimecode(1800, kRate2997, false);
    QVERIFY(forcedNdf.find(';') == std::string::npos);
}

void TestTimecode::parseAcceptsBothSeparators() {
    // Parser is lenient about the frame separator; it does not encode mode.
    QCOMPARE(Timecode::timecodeToFrames("00:00:01:00", kRate30, false), int64_t(30));
    QCOMPARE(Timecode::timecodeToFrames("00:00:01;00", kRate30, false), int64_t(30));
}

void TestTimecode::parseRejectsMalformedInput() {
    // Structurally malformed input returns 0 (documented contract): wrong field
    // count, non-digit/non-separator characters, and empty/no-digit strings.
    QCOMPARE(Timecode::timecodeToFrames("", kRate30, false), int64_t(0));
    QCOMPARE(Timecode::timecodeToFrames("00:00:01", kRate30, false), int64_t(0));       // 3 fields
    QCOMPARE(Timecode::timecodeToFrames("00:00:01:00:00", kRate30, false), int64_t(0)); // 5 fields
    QCOMPARE(Timecode::timecodeToFrames("aa:bb:cc:dd", kRate30, false), int64_t(0));    // non-digit
    QCOMPARE(Timecode::timecodeToFrames("::::", kRate30, false), int64_t(0));           // no digits
}

void TestTimecode::parseOversizedFieldDoesNotOverflow() {
    // A pathologically long numeric field must not trigger signed-integer
    // overflow (UB) — the accumulator is 64-bit and saturates. The exact folded
    // value is unspecified, but the call must complete and stay non-negative
    // (this test runs clean under -fsanitize=undefined in the CI asan-ubsan job).
    const std::string huge(40, '9');
    const int64_t r = Timecode::timecodeToFrames(huge + ":00:00:00", kRate30, false);
    QVERIFY(r >= 0);
    // A well-formed but out-of-range frame field is accepted (lenient parser),
    // not UB and not a structural reject.
    QCOMPARE(Timecode::timecodeToFrames("00:00:00:40", kRate30, false), int64_t(40));
}

QTEST_GUILESS_MAIN(TestTimecode)
#include "tst_timecode.moc"
