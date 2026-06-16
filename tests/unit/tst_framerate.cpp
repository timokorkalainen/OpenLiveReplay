#include <QtTest>

#include "recorder_engine/framerate.h"

class TestFrameRate : public QObject {
    Q_OBJECT
private slots:
    void integerRate();
    void rate2997();
    void roundedFps();
    void samplesPerFrame();
    void parse();
    void label();
};

void TestFrameRate::integerRate() {
    const FrameRate r{30, 1};
    QCOMPARE(r.msForFrame(30), qint64(1000));
    QCOMPARE(r.frameForMs(1000), qint64(30));
    QCOMPARE(r.toDouble(), 30.0);
}

void TestFrameRate::rate2997() {
    const FrameRate r{30000, 1001};
    QCOMPARE(r.msForFrame(30), qint64(1001)); // 30 frames of 29.97 span 1001 ms
    QCOMPARE(r.frameForMs(1001), qint64(30));
    QVERIFY(r.toDouble() > 29.9 && r.toDouble() < 30.0);
}

void TestFrameRate::roundedFps() {
    QCOMPARE((FrameRate{30000, 1001}).roundedFps(), 30);
    QCOMPARE((FrameRate{60000, 1001}).roundedFps(), 60);
    QCOMPARE((FrameRate{25, 1}).roundedFps(), 25);
}

void TestFrameRate::samplesPerFrame() {
    QCOMPARE((FrameRate{30, 1}).samplesPerFrame(48000), qint64(1600));
    QCOMPARE((FrameRate{30000, 1001}).samplesPerFrame(48000), qint64(1601)); // 1601.6 trunc
}

void TestFrameRate::parse() {
    QVERIFY(parseFrameRate("30") == (FrameRate{30, 1}));
    QVERIFY(parseFrameRate("29.97") == (FrameRate{30000, 1001}));
    QVERIFY(parseFrameRate("30000/1001") == (FrameRate{30000, 1001}));
    QVERIFY(parseFrameRate("garbage") == (FrameRate{30, 1}));
    QVERIFY(parseFrameRate("60") == (FrameRate{60, 1}));
    QVERIFY(parseFrameRate("59.94") == (FrameRate{60000, 1001}));
    QVERIFY(parseFrameRate("30.0") == (FrameRate{30, 1})); // integer-exact, not 29.97
    QVERIFY(parseFrameRate("30/0") == (FrameRate{30, 1})); // malformed -> default
}

void TestFrameRate::label() {
    QCOMPARE(frameRateLabel(FrameRate{30000, 1001}), QStringLiteral("29.97"));
    QCOMPARE(frameRateLabel(FrameRate{30, 1}), QStringLiteral("30"));
}

QTEST_GUILESS_MAIN(TestFrameRate)
#include "tst_framerate.moc"
