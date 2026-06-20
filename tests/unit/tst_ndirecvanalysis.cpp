#include <QtTest>

#include "tests/e2e/ndi_recv_analysis.h"

class TestNdiRecvAnalysis : public QObject {
    Q_OBJECT
private slots:
    void continuityCountsDropsDupesReorders();
    void avSyncMeasuresJitterAroundMedianOffset();
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

void TestNdiRecvAnalysis::avSyncMeasuresJitterAroundMedianOffset() {
    // Jitter = max deviation from the MEDIAN signed offset (beep[i]-flash[i]), so the
    // constant NDI audio-buffer delay is normalized away and only A-V drift counts.
    // symmetric small offsets {0,1,0} around median 0 -> max deviation 1
    QCOMPARE(ndiAvSyncMaxFrames({0, 15, 30}, {0, 16, 30}), 1);
    // perfectly locked -> 0
    QCOMPARE(ndiAvSyncMaxFrames({0, 15, 30}, {0, 15, 30}), 0);
    // a CONSTANT 14-frame buffer delay is NOT desync: offsets {14,14,14}, median 14 -> 0.
    // (the old nearest-neighbor logic would have returned 14 here.)
    QCOMPARE(ndiAvSyncMaxFrames({0, 15, 30}, {14, 29, 44}), 0);
    // genuine drift IS caught: offsets {10,0,-8}, median 0 -> max deviation 10.
    QCOMPARE(ndiAvSyncMaxFrames({0, 15, 30}, {10, 15, 22}), 10);
    // empty -> -1
    QCOMPARE(ndiAvSyncMaxFrames({}, {0}), -1);

    // Re-chunking robustness: a ~60ms marker event registers on a VARIABLE number of frames,
    // so beeps[]/flashes[] may hold 1-3 near-adjacent ordinals per event. Collapsing each run
    // to its onset must keep these locked (was the e2e false-FAIL: one extra beep detection
    // slipped the index pairing by a whole period -> avSyncMaxFrames in multiples of 15).
    // beep at frame 15 detected on two adjacent audio frames {15,16}: still 0, not 14.
    QCOMPARE(ndiAvSyncMaxFrames({0, 15, 30}, {0, 15, 16, 30}), 0);
    // a flash detected on two adjacent video frames {0,1} collapses to onset 0: still 0.
    QCOMPARE(ndiAvSyncMaxFrames({0, 1, 15, 30}, {0, 15, 30}), 0);
    // multi-frame detections on BOTH streams every event: still perfectly locked.
    QCOMPARE(ndiAvSyncMaxFrames({0, 1, 15, 16, 30, 31}, {0, 1, 15, 16, 30, 31}), 0);
    // onset-collapse must NOT mask real drift: a genuinely late beep event (onset 24 vs
    // expected ~15) is a distinct event (gap > 2), so offsets {0,9,0}, median 0 -> 9.
    QCOMPARE(ndiAvSyncMaxFrames({0, 15, 30}, {0, 24, 30}), 9);
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
