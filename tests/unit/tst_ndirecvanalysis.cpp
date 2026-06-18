#include <QtTest>

#include "tests/e2e/ndi_recv_analysis.h"

class TestNdiRecvAnalysis : public QObject {
    Q_OBJECT
private slots:
    void continuityCountsDropsDupesReorders();
    void avSyncIsMaxNearestFlashBeepOffset();
    void cadenceReportsMaxGapAndMeanRate();
};

void TestNdiRecvAnalysis::continuityCountsDropsDupesReorders() {
    // perfect run: 0..5
    auto perfect = ndiAnalyzeContinuity({0, 1, 2, 3, 4, 5});
    QCOMPARE(perfect.framesReceived, qint64(6));
    QCOMPARE(perfect.drops, qint64(0));
    QCOMPARE(perfect.dupes, qint64(0));
    QCOMPARE(perfect.reorders, qint64(0));

    // one drop (index 2 missing): 0,1,3,4
    auto dropped = ndiAnalyzeContinuity({0, 1, 3, 4});
    QCOMPARE(dropped.drops, qint64(1));

    // a duplicate frame: 0,1,1,2
    auto duped = ndiAnalyzeContinuity({0, 1, 1, 2});
    QCOMPARE(duped.dupes, qint64(1));

    // a reorder: 0,2,1,3 — step 0->2 and 1->3 are forward jumps counted as drops in this model
    auto reordered = ndiAnalyzeContinuity({0, 2, 1, 3});
    QCOMPARE(reordered.reorders, qint64(1));
    QCOMPARE(reordered.drops,
             qint64(2)); // a reorder inherently registers the forward jumps as drops too
}

void TestNdiRecvAnalysis::avSyncIsMaxNearestFlashBeepOffset() {
    // flashes at 0,15,30 ; beeps at 0,16,30 -> offsets 0,1,0 -> max 1
    QCOMPARE(ndiAvSyncMaxFrames({0, 15, 30}, {0, 16, 30}), 1);
    QCOMPARE(ndiAvSyncMaxFrames({0, 15, 30}, {0, 15, 30}), 0);
    QCOMPARE(ndiAvSyncMaxFrames({}, {0}), -1);
}

void TestNdiRecvAnalysis::cadenceReportsMaxGapAndMeanRate() {
    // 30fps nominal; arrivals every ~33.33ms, one double-gap in the middle
    std::vector<double> arr;
    double t = 0.0;
    for (int i = 0; i < 10; ++i) {
        arr.push_back(t);
        t += (i == 4) ? (2.0 / 30.0) : (1.0 / 30.0); // one 2x gap
    }
    const NdiCadence c = ndiAnalyzeCadence(arr, 30, 1);
    QCOMPARE(c.maxGapFrames, 2);
    QVERIFY(std::abs(c.meanRateHz - 30.0) <=
            3.0); // 10 frames, 1 double-gap → 27 Hz (exactly 3 off)
}

QTEST_GUILESS_MAIN(TestNdiRecvAnalysis)
#include "tst_ndirecvanalysis.moc"
