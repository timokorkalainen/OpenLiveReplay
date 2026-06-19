#include <QtTest>

#include "recorder_engine/replaymanager.h"
#include "recorder_engine/ingest/ingestsession.h"
#include "recorder_engine/timing/smpte12m.h"
#include "recorder_engine/timing/sourceoffsetestimator.h"

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

    // Phase 4 Task 3: reference-source selection + inter-cam phase estimation.
    void referenceIsHighestClockQualityTieLowestIndex();
    void timecodeAlignedSourceGradesFrameAccurate();
    void lockedNoTimecodeSourceGradesBoundedWithOffset();
    void arrivalOnlySourceGradesApproximate();
    void referenceSourceHasZeroPhase();
    void disconnectReselectsReferenceAwayFromDeadSource();

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

    // Drives the production seam ReplayManager::onSourceStatsUpdated exactly as the
    // queued StreamWorker::statsUpdated signal would: by name via the meta-object.
    static bool feedStats(ReplayManager& m, int src, const IngestStats& stats) {
        return QMetaObject::invokeMethod(&m, "onSourceStatsUpdated", Qt::DirectConnection,
                                         Q_ARG(int, src), Q_ARG(IngestStats, stats));
    }

    static IngestStats clockStats(ClockQuality q, bool locked, int64_t offsetNs, double ppm = 0.0) {
        IngestStats s;
        s.clockQuality = int(q);
        s.clockLocked = locked;
        s.clockOffsetNs = offsetNs;
        s.clockPpm = ppm;
        return s;
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

void TestReplayManagerTimecode::referenceIsHighestClockQualityTieLowestIndex() {
    ReplayManager manager;
    // Source 0: NDI-locked. Source 1: PCR-locked (higher quality). Source 2: PCR-locked
    // (ties source 1 on quality). The reference must be the highest ClockQuality, and
    // a quality tie breaks to the LOWEST index → source 1.
    QVERIFY(feedStats(manager, 0, clockStats(ClockQuality::Ndi, true, 0)));
    QVERIFY(feedStats(manager, 1, clockStats(ClockQuality::Pcr, true, 1000000)));
    QVERIFY(feedStats(manager, 2, clockStats(ClockQuality::Pcr, true, 2000000)));
    QCOMPARE(manager.referenceSource(), 1);
}

void TestReplayManagerTimecode::timecodeAlignedSourceGradesFrameAccurate() {
    ReplayManager manager;
    // Both PCR-locked; source 1 is the reference (lowest index would be 0 on a tie,
    // so give source 0 the higher quality to make it the reference). A jam-synced TC
    // pair (equal TC lands on the same session frame) makes source 1 TC-aligned to the
    // reference → FrameAccurate.
    QVERIFY(feedFrameTimecode(manager, 0, tc100ns(1, 0, 0, 0), 100));
    QVERIFY(feedFrameTimecode(manager, 1, tc100ns(1, 0, 0, 0), 100));
    QVERIFY(feedStats(manager, 0, clockStats(ClockQuality::Pcr, true, 0)));
    QVERIFY(feedStats(manager, 1, clockStats(ClockQuality::Pcr, true, 5000000)));
    QCOMPARE(manager.referenceSource(), 0);
    QCOMPARE(manager.sourceTier(1), ConfidenceTier::FrameAccurate);
    QCOMPARE(manager.sourcePhaseBoundMs(1), 0);
    QCOMPARE(manager.sourceTier(0), ConfidenceTier::FrameAccurate); // reference vs itself
}

void TestReplayManagerTimecode::lockedNoTimecodeSourceGradesBoundedWithOffset() {
    ReplayManager manager;
    // Both PCR-locked, NO timecode → Bounded. measuredOffsetMs is the clockOffsetNs
    // difference (source - reference) / 1e6. Reference is source 0 (tie → lowest index).
    // Source 1 leads the reference by 7 ms (7e6 ns).
    QVERIFY(feedStats(manager, 0, clockStats(ClockQuality::Pcr, true, 0)));
    QVERIFY(feedStats(manager, 1, clockStats(ClockQuality::Pcr, true, 7000000)));
    QCOMPARE(manager.referenceSource(), 0);
    QCOMPARE(manager.sourceTier(1), ConfidenceTier::Bounded);
    QCOMPARE(manager.sourcePhaseOffsetMs(1), int64_t(7));
    QVERIFY(manager.sourcePhaseBoundMs(1) >= 4); // at least the base bound
}

void TestReplayManagerTimecode::arrivalOnlySourceGradesApproximate() {
    ReplayManager manager;
    // Source 0 is PCR-locked (reference). Source 1 is arrival-only, unlocked → Approximate.
    QVERIFY(feedStats(manager, 0, clockStats(ClockQuality::Pcr, true, 0)));
    QVERIFY(feedStats(manager, 1, clockStats(ClockQuality::Arrival, false, 0)));
    QCOMPARE(manager.referenceSource(), 0);
    QCOMPARE(manager.sourceTier(1), ConfidenceTier::Approximate);
    QCOMPARE(manager.sourcePhaseBoundMs(1), 40);
}

void TestReplayManagerTimecode::referenceSourceHasZeroPhase() {
    ReplayManager manager;
    // The reference's own offset is 0 by construction (it is differenced against itself),
    // even though its absolute clockOffsetNs is non-zero.
    QVERIFY(feedStats(manager, 0, clockStats(ClockQuality::Pcr, true, 9000000)));
    QVERIFY(feedStats(manager, 1, clockStats(ClockQuality::Ndi, true, 1000000)));
    QCOMPARE(manager.referenceSource(), 0);
    QCOMPARE(manager.sourcePhaseOffsetMs(0), int64_t(0));
}

void TestReplayManagerTimecode::disconnectReselectsReferenceAwayFromDeadSource() {
    ReplayManager manager;
    // Source 0: NDI-locked. Source 1: PCR-locked (higher quality) → reference is 1.
    QVERIFY(feedStats(manager, 0, clockStats(ClockQuality::Ndi, true, 0)));
    QVERIFY(feedStats(manager, 1, clockStats(ClockQuality::Pcr, true, 1000000)));
    QCOMPARE(manager.referenceSource(), 1);
    // Source 1 disconnects: it must drop from eligibility so the reference re-selects
    // to the still-live source 0 (the servo must never correct toward a dead reference).
    QVERIFY(QMetaObject::invokeMethod(&manager, "onSourcePhaseConnectionChanged",
                                      Qt::DirectConnection, Q_ARG(int, 1), Q_ARG(bool, false)));
    QCOMPARE(manager.referenceSource(), 0);
}

QTEST_GUILESS_MAIN(TestReplayManagerTimecode)
#include "tst_replaymanager_timecode.moc"
