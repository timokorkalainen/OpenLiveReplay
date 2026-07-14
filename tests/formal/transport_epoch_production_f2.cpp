#include <QtCore>

#include "playback/frameprovider.h"
#include "playback/playbacktransport.h"

// Access control is intentionally relaxed only in this proof translation unit.
// The PlaybackWorker implementation object remains production-shaped and is
// compiled with OLR_UNIT_TEST undefined.
#define private public
#include "playback/playbackworker.h"
#undef private

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

#ifdef OLR_UNIT_TEST
#error "transport epoch production F2 proof must not compile with OLR_UNIT_TEST"
#endif

namespace {
FrameHandle video(qint64 pts, uchar y) {
    FrameHandle frame = solidYuv420pHandle(4, 4, y, 128, 128);
    frame.metadata().key.feedIndex = 0;
    frame.metadata().key.ptsMs = pts;
    return frame;
}

class CollectingSink final : public IOutputSink {
public:
    OutputTargetKind kind() const override { return OutputTargetKind::QtPreview; }

    bool start(const OutputTargetAssignment& assignment, FrameRate rate) override {
        QMutexLocker locker(&m_mutex);
        m_active = assignment.enabled && rate.isValid();
        return m_active;
    }

    void stop() override {
        QMutexLocker locker(&m_mutex);
        m_active = false;
    }

    bool isActive() const override {
        QMutexLocker locker(&m_mutex);
        return m_active;
    }

    bool submit(const OutputBusFrame& frame) override {
        QMutexLocker locker(&m_mutex);
        if (!m_active) return false;
        m_frames.append(frame);
        return true;
    }

    QVector<OutputBusFrame> frames() const {
        QMutexLocker locker(&m_mutex);
        return m_frames;
    }

private:
    mutable QMutex m_mutex;
    bool m_active = false;
    QVector<OutputBusFrame> m_frames;
};

int fail(const char* reason) {
    std::cerr << "transport epoch production F2 proof: FAIL: " << reason << '\n';
    return 1;
}
} // namespace

int main() {
    FrameProvider feed;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    CollectingSink sink;
    PlaybackWorker worker({&feed}, &transport);
    worker.m_outputFeedCount = 1;
    worker.m_outputWidth = 4;
    worker.m_outputHeight = 4;
    worker.m_selectedOutputFeed.store(0, std::memory_order_relaxed);
    worker.m_seekGeneration.store(8, std::memory_order_release);
    worker.m_committedGeneration.store(7, std::memory_order_release);

    OutputRuntime* runtime = nullptr;
    {
        QMutexLocker runtimeLocker(&worker.m_outputRuntimeMutex);
        worker.m_outputRuntime =
            std::make_unique<OutputRuntime>(FrameRate::fromFraction(25, 1), 1, 4, 4);
        runtime = worker.m_outputRuntime.get();
    }

    OutputFrameCache staleCache(1, 4, 4);
    staleCache.insertVideoFrame(video(100, 35));
    OutputFrameCache currentCache(1, 4, 4);
    currentCache.insertVideoFrame(video(500, 95));

    OutputTargetAssignment assignment;
    assignment.id = QStringLiteral("feed0-preview");
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.kind = OutputTargetKind::QtPreview;
    assignment.enabled = true;

    std::atomic<int> snapshotCalls{0};
    std::mutex snapshotMutex;
    std::condition_variable snapshotCapturedCv;
    std::condition_variable snapshotReleaseCv;
    bool snapshotCaptured = false;
    bool releaseSnapshot = false;
    bool diagnosticTimeout = false;
    runtime->setSnapshotProvider([&]() {
        OutputRuntimeSnapshot snapshot;
        snapshot.state.playing = true;
        snapshot.state.selectedFeedIndex = 0;
        const int call = snapshotCalls.fetch_add(1, std::memory_order_acq_rel);
        if (call == 0) {
            snapshot.cache = staleCache;
            snapshot.state.playheadMs = 100;
            std::unique_lock<std::mutex> lock(snapshotMutex);
            snapshotCaptured = true;
            snapshotCapturedCv.notify_all();
            if (!snapshotReleaseCv.wait_for(lock, std::chrono::seconds(2),
                                            [&]() { return releaseSnapshot; }))
                diagnosticTimeout = true;
        } else {
            snapshot.cache = currentCache;
            snapshot.state.playheadMs = 500;
        }
        return snapshot;
    });
    runtime->setEndpoints({{assignment, &sink}});

    std::thread preCommitDispatch([&]() { runtime->dispatchImmediate(); });
    bool capturedBeforeCommit = false;
    {
        std::unique_lock<std::mutex> lock(snapshotMutex);
        capturedBeforeCommit = snapshotCapturedCv.wait_for(lock, std::chrono::seconds(2),
                                                           [&]() { return snapshotCaptured; });
    }

    PlaybackWorker::OutputCommitResult result;
    if (capturedBeforeCommit) {
        QMutexLocker locker(&worker.m_mutex);
        worker.m_seekTargetMs = -1;
        QMutexLocker bufferLocker(&worker.m_bufferMutex);
        worker.m_outputCache = std::make_unique<OutputFrameCache>(1, 4, 4);
        worker.m_outputCache->insertVideoFrame(video(500, 95));

        PlaybackWorker::OutputCommit commit;
        commit.playheadMs = 500;
        commit.seekGeneration = 8;
        commit.cacheAction = PlaybackWorker::OutputCacheAction::Publish;
        commit.coverageMode = PlaybackWorker::OutputCoverageMode::OperatorSeek;
        commit.guardPlayheadCache = true;
        result = worker.commitOutputStateLocked(commit);
    }
    {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        releaseSnapshot = true;
        snapshotReleaseCv.notify_all();
    }
    preCommitDispatch.join();
    runtime->dispatchImmediate();

    if (!capturedBeforeCommit) return fail("snapshot provider did not reach capture barrier");
    if (diagnosticTimeout) return fail("snapshot provider timed out waiting for release");
    if (!result.committed) return fail("central output commit was rejected");
    if (snapshotCalls.load(std::memory_order_acquire) != 2)
        return fail("expected one pre-commit and one post-commit snapshot");
    const QVector<OutputBusFrame> frames = sink.frames();
    if (frames.size() != 1) return fail("stale pre-commit snapshot was dispatched");
    if (frames.first().sampledPlayheadMs != 500 || frames.first().video.metadata().key.ptsMs != 500)
        return fail("post-commit dispatch did not use the current snapshot");

    std::cout << "transport epoch production F2 proof: PASS "
                 "(OLR_UNIT_TEST undefined, central commit reset rejected stale lease)\n";
    return 0;
}
