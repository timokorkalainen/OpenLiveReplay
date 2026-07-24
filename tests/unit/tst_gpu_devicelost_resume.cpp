#include <QtTest>

#include "playback/gpu/gpudevicelossmonitor.h"
#include "playback/gpu/gpugeneration.h"
#include "playback/output/framehandle.h"

class TestGpuDeviceLostResume : public QObject {
    Q_OBJECT
private slots:
    void rebuildResumesUnderBumpedGeneration();
};

void TestGpuDeviceLostResume::rebuildResumesUnderBumpedGeneration() {
    auto& monitor = GpuDeviceLossMonitor::instance();
    monitor.reset();
    GpuGenerationCounter::instance().resetForTest();

    const uint64_t mintedGeneration = GpuGenerationCounter::instance().bump();
    FrameHandle preLoss = solidYuv420pHandle(16, 16, 16, 128, 128);
    preLoss.metadata().gpuGeneration = mintedGeneration;
    QVERIFY(!preLoss.isStaleForGeneration(GpuGenerationCounter::instance().current()));

    const uint64_t participant = monitor.registerRecoveryParticipant();
    QVERIFY(participant != 0);
    const uint64_t lossGeneration = monitor.recordLoss();
    QVERIFY(monitor.isLost());
    QVERIFY(preLoss.isStaleForGeneration(GpuGenerationCounter::instance().current()));

    QVERIFY(monitor.acknowledgeRecoveryCleanup(participant, lossGeneration));
    const GpuRecoveryTicket ticket = monitor.beginRebuild(participant);
    QVERIFY(ticket.isValid());
    QVERIFY(monitor.clearForRebuild(ticket));
    QVERIFY(!monitor.isLost());
    QVERIFY(monitor.unregisterRecoveryParticipant(participant).has_value());
    const uint64_t resumeGeneration = GpuGenerationCounter::instance().current();
    FrameHandle postRebuild = solidYuv420pHandle(16, 16, 16, 128, 128);
    postRebuild.metadata().gpuGeneration = resumeGeneration;

    QVERIFY(!postRebuild.isStaleForGeneration(resumeGeneration));
    QVERIFY(preLoss.isStaleForGeneration(resumeGeneration));
}

QTEST_GUILESS_MAIN(TestGpuDeviceLostResume)
#include "tst_gpu_devicelost_resume.moc"
