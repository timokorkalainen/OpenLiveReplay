#include <QtTest>

#include "recorder_engine/timing/audioservo.h"

class TestAudioServo : public QObject {
    Q_OBJECT

private slots:
    void correctedSamples();
    void clampsPpm();
    void fractionalCarryMakesSmallPpmVisible();
};

void TestAudioServo::correctedSamples() {
    QCOMPARE(correctedSrcSamples(48000, 0.0), int64_t(48000));
    QCOMPARE(correctedSrcSamples(48000, 200.0), int64_t(48010));
    QCOMPARE(correctedSrcSamples(48000, -200.0), int64_t(47990));
}

void TestAudioServo::clampsPpm() {
    QCOMPARE(clampPpm(5000.0, 500.0), 500.0);
    QCOMPARE(clampPpm(-5000.0, 500.0), -500.0);
    QCOMPARE(clampPpm(100.0, 500.0), 100.0);
}

void TestAudioServo::fractionalCarryMakesSmallPpmVisible() {
    double carry = 0.0;
    int64_t total = 0;
    for (int i = 0; i < 5; ++i) {
        total += correctedSrcSamplesAccumulated(1600, 200.0, &carry);
    }
    QCOMPARE(total, int64_t(8002));

    carry = 0.0;
    total = 0;
    for (int i = 0; i < 5; ++i) {
        total += correctedSrcSamplesAccumulated(1600, -200.0, &carry);
    }
    QCOMPARE(total, int64_t(7998));
}

QTEST_GUILESS_MAIN(TestAudioServo)
#include "tst_audioservo.moc"
