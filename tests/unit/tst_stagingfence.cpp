#include <QtTest>

#include "playback/gpu/gpufence.h"
#include "playback/playbacktransport.h"
#include "playback/playbackworker.h"

#include <atomic>

class TestStagingFence : public QObject {
    Q_OBJECT
private slots:
    void swapWaitsForStagingFence();
    void workerCutDefersUntilStagingFenceCompletes();
    void windowsGpuImportRequiresBothFences();
};

class ManualFence final : public GpuFence {
public:
    uint64_t signal() override { return m_completed.fetch_add(1, std::memory_order_acq_rel) + 1; }
    bool wait(uint64_t value, int) override { return completedValue() >= value; }
    uint64_t completedValue() const override { return m_completed.load(std::memory_order_acquire); }
    void complete(uint64_t value) { m_completed.store(value, std::memory_order_release); }

private:
    std::atomic<uint64_t> m_completed{0};
};

class ScopedEnv {
public:
    ScopedEnv(const char* name, const QByteArray& value)
        : m_name(name), m_hadValue(qEnvironmentVariableIsSet(name)), m_previous(qgetenv(name)) {
        qputenv(m_name, value);
    }

    ~ScopedEnv() {
        if (m_hadValue)
            qputenv(m_name, m_previous);
        else
            qunsetenv(m_name);
    }

private:
    const char* m_name = nullptr;
    bool m_hadValue = false;
    QByteArray m_previous;
};

void TestStagingFence::swapWaitsForStagingFence() {
    auto fence = GpuFence::create();
    if (!fence) QSKIP("no GPU fence backend");

    QVERIFY(!fence->wait(1, 50));
    const uint64_t staged = fence->signal();
    QVERIFY(fence->wait(staged, 1000));
}

void TestStagingFence::workerCutDefersUntilStagingFenceCompletes() {
    ScopedEnv gpuEnabled("OLR_GPU_PIPELINE", "1");
    PlaybackTransport transport;
    PlaybackWorker worker({}, &transport);
    auto stagingFence = std::make_shared<ManualFence>();

    worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 4, 4);
    worker.m_prerollStagingCache = std::make_unique<OutputFrameCache>(1, 4, 4);
    worker.m_cutArmed.store(true, std::memory_order_release);
    worker.m_stagingCovers.store(true, std::memory_order_release);
    worker.m_scheduledCutFrame.store(5, std::memory_order_release);
    worker.m_scheduledCutTargetMs.store(1000, std::memory_order_release);
    worker.m_armSeekGen.store(worker.m_seekGeneration.load(std::memory_order_acquire),
                              std::memory_order_release);
    worker.m_stagingFence = stagingFence;
    worker.m_stagedFenceValue.store(1, std::memory_order_release);

    {
        QMutexLocker locker(&worker.m_bufferMutex);
        worker.maybeFireScheduledCut(5);
    }
    QCOMPARE(worker.cutsFired(), 0);
    QCOMPARE(worker.m_scheduledCutFrame.load(std::memory_order_acquire), qint64(5));
    QCOMPARE(transport.currentPos(), int64_t(0));

    stagingFence->complete(1);
    {
        QMutexLocker locker(&worker.m_bufferMutex);
        worker.maybeFireScheduledCut(5);
    }
    QCOMPARE(worker.cutsFired(), 1);
    QCOMPARE(worker.m_scheduledCutFrame.load(std::memory_order_acquire), qint64(-1));
    QCOMPARE(transport.currentPos(), int64_t(1000));
}

void TestStagingFence::windowsGpuImportRequiresBothFences() {
    PlaybackTransport transport;
    PlaybackWorker worker({}, &transport);

    QVERIFY(!worker.ensureWindowsGpuImportFencesReadyForDecode(nullptr));
}

QTEST_GUILESS_MAIN(TestStagingFence)
#include "tst_stagingfence.moc"
