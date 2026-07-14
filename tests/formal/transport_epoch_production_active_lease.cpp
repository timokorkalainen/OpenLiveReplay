#include <QtCore>

#include "playback/frameprovider.h"
#include "playback/playbacktransport.h"

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
#error "transport epoch active-lease proof must not compile with OLR_UNIT_TEST"
#endif

namespace {
FrameHandle video(qint64 pts, uchar y) {
    FrameHandle frame = solidYuv420pHandle(4, 4, y, 128, 128);
    frame.metadata().key.feedIndex = 0;
    frame.metadata().key.ptsMs = pts;
    return frame;
}

class BlockingSink final : public IOutputSink {
public:
    OutputTargetKind kind() const override { return OutputTargetKind::QtPreview; }

    bool start(const OutputTargetAssignment& assignment, FrameRate rate) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_active = assignment.enabled && rate.isValid();
        return m_active;
    }

    void stop() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_active = false;
        m_released = true;
        m_release.notify_all();
    }

    bool isActive() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_active;
    }

    bool submit(const OutputBusFrame& frame) override {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_active) return false;
        m_frames.append(frame);
        m_discardsAtSubmit.append(m_discardPendingCalls.load(std::memory_order_acquire));
        if (++m_submitCount != 2) return true;
        m_insideSubmit = true;
        m_entered.notify_all();
        if (!m_release.wait_for(lock, std::chrono::seconds(2), [this]() { return m_released; })) {
            m_diagnosticTimeout = true;
            return false;
        }
        return true;
    }

    void discardPending() override {
        m_discardPendingCalls.fetch_add(1, std::memory_order_acq_rel);
    }

    bool waitUntilInsideSubmit() {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_entered.wait_for(lock, std::chrono::seconds(2),
                                  [this]() { return m_insideSubmit; });
    }

    void release() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_released = true;
        m_release.notify_all();
    }

    bool diagnosticTimeout() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_diagnosticTimeout;
    }

    QVector<OutputBusFrame> frames() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_frames;
    }

    QVector<int> discardsAtSubmit() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_discardsAtSubmit;
    }

    int discardPendingCalls() const {
        return m_discardPendingCalls.load(std::memory_order_acquire);
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_entered;
    std::condition_variable m_release;
    bool m_active = false;
    bool m_insideSubmit = false;
    bool m_released = false;
    bool m_diagnosticTimeout = false;
    int m_submitCount = 0;
    QVector<OutputBusFrame> m_frames;
    QVector<int> m_discardsAtSubmit;
    std::atomic<int> m_discardPendingCalls{0};
};

int fail(const char* reason) {
    std::cerr << "transport epoch active-lease proof: FAIL: " << reason << '\n';
    return 1;
}
} // namespace

int main() {
    FrameProvider feed;
    PlaybackTransport transport;
    transport.setFrameRate(25, 1);
    BlockingSink sink;
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

    OutputFrameCache seedCache(1, 4, 4);
    seedCache.insertVideoFrame(video(100, 35));
    OutputFrameCache activeCache(1, 4, 4);
    activeCache.insertVideoFrame(video(140, 75));
    OutputFrameCache followerCache(1, 4, 4);
    std::atomic<int> snapshotStage{0};
    runtime->setSnapshotProvider([&]() {
        OutputRuntimeSnapshot snapshot;
        snapshot.state.playing = true;
        snapshot.state.selectedFeedIndex = 0;
        const int stage = snapshotStage.load(std::memory_order_acquire);
        if (stage == 0) {
            snapshot.cache = seedCache;
            snapshot.state.playheadMs = 100;
        } else if (stage == 1) {
            snapshot.cache = activeCache;
            snapshot.state.playheadMs = 140;
        } else {
            snapshot.cache = followerCache;
            snapshot.state.playheadMs = 500;
        }
        return snapshot;
    });

    OutputTargetAssignment assignment;
    assignment.id = QStringLiteral("feed0-preview");
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.kind = OutputTargetKind::QtPreview;
    assignment.enabled = true;
    runtime->setEndpoints({{assignment, &sink}});
    runtime->setIdentitySkip(false);
    runtime->dispatchImmediate();

    snapshotStage.store(1, std::memory_order_release);
    std::thread activeDispatch([&]() { runtime->dispatchImmediate(); });
    const bool activeLeaseEntered = sink.waitUntilInsideSubmit();

    PlaybackWorker::OutputCommitResult commitResult;
    std::mutex commitMutex;
    std::condition_variable commitReturnedCv;
    bool commitReturned = false;
    std::thread commitThread([&]() {
        {
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
            commitResult = worker.commitOutputStateLocked(commit);
        }
        {
            std::lock_guard<std::mutex> lock(commitMutex);
            commitReturned = true;
        }
        commitReturnedCv.notify_all();
    });

    bool commitReturnedBeforeRelease = false;
    if (activeLeaseEntered) {
        std::unique_lock<std::mutex> lock(commitMutex);
        commitReturnedBeforeRelease = commitReturnedCv.wait_for(lock, std::chrono::seconds(2),
                                                                [&]() { return commitReturned; });
    }
    const int discardsBeforeRelease = sink.discardPendingCalls();

    snapshotStage.store(2, std::memory_order_release);
    std::thread followerDispatch([&]() { runtime->dispatchImmediate(); });
    sink.release();
    activeDispatch.join();
    commitThread.join();
    followerDispatch.join();

    if (!activeLeaseEntered) return fail("active sink submission did not reach lease barrier");
    if (!commitReturnedBeforeRelease) return fail("commit blocked behind active sink lease");
    if (!commitResult.committed) return fail("central output commit was rejected");
    if (sink.diagnosticTimeout()) return fail("active sink timed out waiting for release");
    if (discardsBeforeRelease != 0) return fail("reset applied before active sink release");
    const QVector<OutputBusFrame> frames = sink.frames();
    if (frames.size() != 3) return fail("expected seed, active, and follower submissions");
    const QVector<int> discardsAtSubmit = sink.discardsAtSubmit();
    if (discardsAtSubmit.size() != 3 || discardsAtSubmit.last() != 1)
        return fail("pending reset was not applied before follower lease");
    const OutputBusFrame& follower = frames.last();
    if (follower.sampledPlayheadMs != 500)
        return fail("follower lease sampled the wrong committed playhead");

    std::cout << "transport epoch active-lease proof: PASS "
                 "(commit nonblocking, pending reset precedes follower lease)\n";
    return 0;
}
