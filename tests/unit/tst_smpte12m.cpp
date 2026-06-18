#include <QtTest>
#include "recorder_engine/timing/smpte12m.h"

class TestSmpte12m : public QObject {
    Q_OBJECT
private slots:
    void packedRoundTrips();
    void formatNonDrop();
    void formatDrop();
    void frameCountNonDrop();
    void frameCountWraps24h();
    void to100nsMatchesFrameCount();
    void from100nsRoundTrips();
    void invalidFormatsEmpty();
    void parseTimecodeStringRoundTrips();
};

void TestSmpte12m::packedRoundTrips() {
    const Smpte12mTimecode tc{10, 11, 12, 13, /*drop*/ false, /*valid*/ true};
    const uint32_t w = Smpte12m::toPackedWord(tc);
    const Smpte12mTimecode back = Smpte12m::fromPackedWord(w);
    QCOMPARE(back.hours, 10);
    QCOMPARE(back.minutes, 11);
    QCOMPARE(back.seconds, 12);
    QCOMPARE(back.frames, 13);
    QVERIFY(!back.dropFrame);
    QVERIFY(back.valid);
}
void TestSmpte12m::formatDrop() {
    char buf[12];
    Smpte12m::format(Smpte12mTimecode{1, 0, 0, 2, /*drop*/ true, /*valid*/ true}, buf);
    QCOMPARE(QString::fromLatin1(buf), QStringLiteral("01:00:00;02"));
}
void TestSmpte12m::formatNonDrop() {
    char buf[12];
    Smpte12m::format(Smpte12mTimecode{10, 11, 12, 13, false, true}, buf);
    QCOMPARE(QString::fromLatin1(buf), QStringLiteral("10:11:12:13"));
}
void TestSmpte12m::frameCountNonDrop() {
    // 00:00:01:00 at 25 fps = 25 frames.
    QCOMPARE(Smpte12m::toFrameCount(Smpte12mTimecode{0, 0, 1, 0, false, true}, 25), int64_t(25));
    // 00:01:00:00 at 30 fps = 1800 frames.
    QCOMPARE(Smpte12m::toFrameCount(Smpte12mTimecode{0, 1, 0, 0, false, true}, 30), int64_t(1800));
}
void TestSmpte12m::frameCountWraps24h() {
    // 24:00:00:00 wraps to 0.
    QCOMPARE(Smpte12m::toFrameCount(Smpte12mTimecode{24, 0, 0, 0, false, true}, 30), int64_t(0));
}
void TestSmpte12m::to100nsMatchesFrameCount() {
    const Smpte12mTimecode tc{0, 0, 1, 0, false, true};       // 1 second
    QCOMPARE(Smpte12m::to100ns(tc, 25), int64_t(10'000'000)); // 1 s in 100 ns
}
void TestSmpte12m::from100nsRoundTrips() {
    const Smpte12mTimecode tc = Smpte12m::from100ns(10'000'000, 25); // 1 s
    QCOMPARE(tc.seconds, 1);
    QCOMPARE(tc.frames, 0);
    QVERIFY(tc.valid);

    // Round-trip at the NTSC/film rates where 1e7/fps is NOT an integer (24/30/
    // 60). Plain truncation in from100ns loses up to a full frame and drifts with
    // elapsed time; rounding must keep to100ns->from100ns exact at every frame.
    for (const int fps : {24, 30, 60}) {
        for (const auto& src : {Smpte12mTimecode{0, 0, 1, 7, false, true},
                                Smpte12mTimecode{1, 2, 3, fps - 1, false, true},
                                Smpte12mTimecode{0, 10, 0, 1, false, true}}) {
            const Smpte12mTimecode rt = Smpte12m::from100ns(Smpte12m::to100ns(src, fps), fps);
            QCOMPARE(rt.frames, src.frames);
            QCOMPARE(rt.seconds, src.seconds);
            QCOMPARE(rt.minutes, src.minutes);
            QCOMPARE(rt.hours, src.hours);
        }
    }
}
void TestSmpte12m::invalidFormatsEmpty() {
    char buf[12];
    Smpte12m::format(Smpte12mTimecode{}, buf); // valid=false
    QCOMPARE(QString::fromLatin1(buf), QString());
}
void TestSmpte12m::parseTimecodeStringRoundTrips() {
    // Non-drop "HH:MM:SS:FF" parses to fields with dropFrame=false.
    const Smpte12mTimecode nd = Smpte12m::parseTimecodeString("10:11:12:13");
    QVERIFY(nd.valid);
    QCOMPARE(nd.hours, 10);
    QCOMPARE(nd.minutes, 11);
    QCOMPARE(nd.seconds, 12);
    QCOMPARE(nd.frames, 13);
    QVERIFY(!nd.dropFrame);

    // The ';' frame separator flags drop-frame.
    const Smpte12mTimecode df = Smpte12m::parseTimecodeString("01:00:00;02");
    QVERIFY(df.valid);
    QCOMPARE(df.hours, 1);
    QCOMPARE(df.frames, 2);
    QVERIFY(df.dropFrame);

    // Malformed / out-of-range / null inputs are rejected (valid=false), never
    // crash — AMF timecode is strictly best-effort.
    QVERIFY(!Smpte12m::parseTimecodeString(nullptr).valid);
    QVERIFY(!Smpte12m::parseTimecodeString("").valid);
    QVERIFY(!Smpte12m::parseTimecodeString("not a timecode").valid);
    QVERIFY(!Smpte12m::parseTimecodeString("10:11:12").valid);       // too few fields
    QVERIFY(!Smpte12m::parseTimecodeString("10-11-12-13").valid);    // wrong separators
    QVERIFY(!Smpte12m::parseTimecodeString("99:11:12:13").valid);    // hours out of range
    QVERIFY(!Smpte12m::parseTimecodeString("10:61:12:13").valid);    // minutes out of range
    QVERIFY(!Smpte12m::parseTimecodeString("1:2:3:4").valid);        // non-two-digit fields
    QVERIFY(!Smpte12m::parseTimecodeString("10:11:12:13:14").valid); // trailing junk
}
QTEST_GUILESS_MAIN(TestSmpte12m)
#include "tst_smpte12m.moc"
