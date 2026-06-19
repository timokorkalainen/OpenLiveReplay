#include <QtTest>

#include "recorder_engine/replaymanager.h"
#include "recorder_engine/timing/smpte12m.h"

// Exercises the production seam ReplayManager::onFrameTimecode -> m_tcAligner.observe
// and the public sourcesFrameAligned()/sourceFrameOffset() queries that Phase 4
// consumes. onFrameTimecode is a private slot, so it is driven exactly as the
// queued StreamWorker::frameTimecode signal would drive it: by name via the
// meta-object, which is the same dispatch the real connect() uses.
class TestReplayManagerTimecode : public QObject {
    Q_OBJECT

private slots:
    void jamSyncedSourcesReportAligned();
    void offsetSourcesReportNotAlignedAndFrameOffset();
    void noTimecodeSourcesAreNotAligned();

private:
    static int64_t tc100ns(int h, int m, int s, int f) {
        // Producers (SRT/RTMP) encode each frame's TC to 100 ns with the shared
        // nominal fps; the aligner decodes it back with the same constant.
        return Smpte12m::to100ns(Smpte12mTimecode{h, m, s, f, /*drop*/ false, /*valid*/ true},
                                 Smpte12m::kTimecodeNominalFps);
    }

    static bool feedFrameTimecode(ReplayManager& m, int src, int64_t tc100, int64_t frame) {
        return QMetaObject::invokeMethod(&m, "onFrameTimecode", Qt::DirectConnection,
                                         Q_ARG(int, src), Q_ARG(int64_t, tc100),
                                         Q_ARG(int64_t, frame));
    }
};

void TestReplayManagerTimecode::jamSyncedSourcesReportAligned() {
    ReplayManager manager;
    // Two jam-synced sources: 01:00:00:00 lands on the SAME session frame 100.
    QVERIFY(feedFrameTimecode(manager, 0, tc100ns(1, 0, 0, 0), 100));
    QVERIFY(feedFrameTimecode(manager, 1, tc100ns(1, 0, 0, 0), 100));

    QVERIFY(manager.sourcesFrameAligned(0, 1));
    QCOMPARE(manager.sourceFrameOffset(0, 1), int64_t(0));
}

void TestReplayManagerTimecode::offsetSourcesReportNotAlignedAndFrameOffset() {
    ReplayManager manager;
    // Same TC, but source 1's TC arrived 3 session frames LATER than source 0's.
    QVERIFY(feedFrameTimecode(manager, 0, tc100ns(1, 0, 0, 0), 100));
    QVERIFY(feedFrameTimecode(manager, 1, tc100ns(1, 0, 0, 0), 103));

    QVERIFY(!manager.sourcesFrameAligned(0, 1));
    QCOMPARE(manager.sourceFrameOffset(0, 1), int64_t(-3)); // pull B back 3 frames
}

void TestReplayManagerTimecode::noTimecodeSourcesAreNotAligned() {
    ReplayManager manager;
    // Only source 0 ever carried TC; -1 timecodes are ignored by the aligner.
    QVERIFY(feedFrameTimecode(manager, 0, tc100ns(1, 0, 0, 0), 100));
    QVERIFY(feedFrameTimecode(manager, 1, int64_t(-1), 100));

    QVERIFY(!manager.sourcesFrameAligned(0, 1));
    QCOMPARE(manager.sourceFrameOffset(0, 1), int64_t(0));
}

QTEST_GUILESS_MAIN(TestReplayManagerTimecode)
#include "tst_replaymanager_timecode.moc"
