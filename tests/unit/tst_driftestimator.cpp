#include <QtTest>

#include "recorder_engine/timing/driftestimator.h"

#include <cmath>

class TestDriftEstimator : public QObject {
    Q_OBJECT

private slots:
    void unlockedUntilEnoughSamples();
    void zeroDriftIsZeroPpm();
    void positiveSkewMeasured();
    void negativeSkewMeasured();
    void resetClears();
};

namespace {
void feed(DriftEstimator& estimator, double ppm, int count,
          int64_t stepNs = 33366667) {
    for (int i = 0; i < count; ++i) {
        const int64_t sessionNs = int64_t(i) * stepNs;
        const int64_t senderNs =
            int64_t(std::llround(double(i) * double(stepNs) * (1.0 + ppm / 1000000.0)));
        estimator.addSample(senderNs, sessionNs);
    }
}
} // namespace

void TestDriftEstimator::unlockedUntilEnoughSamples() {
    DriftEstimator estimator;
    estimator.addSample(0, 0);
    estimator.addSample(1000, 1000);
    QVERIFY(!estimator.locked());
}

void TestDriftEstimator::zeroDriftIsZeroPpm() {
    DriftEstimator estimator;
    feed(estimator, 0.0, 300);
    QVERIFY(estimator.locked());
    QVERIFY(std::abs(estimator.ppm()) < 1.0);
}

void TestDriftEstimator::positiveSkewMeasured() {
    DriftEstimator estimator;
    feed(estimator, 200.0, 300);
    QVERIFY(estimator.locked());
    QVERIFY(std::abs(estimator.ppm() - 200.0) < 5.0);
}

void TestDriftEstimator::negativeSkewMeasured() {
    DriftEstimator estimator;
    feed(estimator, -120.0, 300);
    QVERIFY(estimator.locked());
    QVERIFY(std::abs(estimator.ppm() + 120.0) < 5.0);
}

void TestDriftEstimator::resetClears() {
    DriftEstimator estimator;
    feed(estimator, 200.0, 300);
    QVERIFY(estimator.locked());
    estimator.reset();
    QVERIFY(!estimator.locked());
    QCOMPARE(estimator.ppm(), 0.0);
}

QTEST_GUILESS_MAIN(TestDriftEstimator)
#include "tst_driftestimator.moc"
