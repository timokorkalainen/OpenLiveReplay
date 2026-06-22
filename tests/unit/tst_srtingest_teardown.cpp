// Regression test for the native SRT ingest teardown race.
//
// NativeSrtIngestSession::run() executes on a dedicated capture thread and uses
// the session's socket every poll (srt_recv / srt_bstats / srt_getsockstate on
// m_socket). StreamWorker::stop() calls requestStop() from a DIFFERENT (worker /
// control) thread on every record-stop, source change, and app quit. If
// requestStop() closes the socket itself (srt_close(m_socket) +
// releaseSrtLibrary()/srt_cleanup()), that cross-thread close races the capture
// thread's concurrent use of the same non-atomic socket handle — a use-after-free
// + data race inside libsrt. The RTMP sibling is already flag-only and the NDI
// sibling was fixed the same way (commit a50e0af); SRT must match: requestStop()
// is flag-only and the socket is closed only on the capture thread (run() / the
// destructor).
//
// The primary check is deterministic and runs in every build: after a real
// loopback connection, requestStop() must leave the socket OPEN (it only sets the
// stop flag). The buggy cross-thread close fails it immediately. A second case
// drives the real run()/requestStop() overlap and asserts a clean, crash-free
// teardown.

#include <QtTest>
#include <QUrl>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "recorder_engine/ingest/ingestsession.h"
#include "recorder_engine/ingest/nativesrtingestsession.h"

namespace {

// SRT_INVALID_SOCK is -1 and m_socket is initialised to -1; a connected socket is
// a non-negative handle. Kept local so the test needs no libsrt headers.
constexpr int kInvalidSrtSocket = -1;

// A loopback SRT link built from two production sessions: a listener-mode acceptor
// (its open() blocks until a caller arrives, so it runs on its own thread) and a
// caller (the session under test). The acceptor holds the accepted connection open
// for the lifetime of the link.
struct LoopbackSrt {
    std::atomic<bool> listenerRunning{true};
    std::atomic<bool> callerRunning{true};
    NativeSrtIngestSession listener;
    std::unique_ptr<NativeSrtIngestSession> caller;
    bool ok = false;

    explicit LoopbackSrt(quint16 port) : listener(/*sourceIndex=*/1, 1920, 1080, &listenerRunning) {
        const QUrl listenerUrl(
            QStringLiteral("srt://127.0.0.1:%1?mode=listener&transtype=live").arg(port));
        const QUrl callerUrl(QStringLiteral("srt://127.0.0.1:%1?transtype=live").arg(port));

        std::atomic<bool> listenerOpened{false};
        std::thread listenerThread([&]() {
            IngestCallbacks cb;
            listenerOpened.store(listener.open(listenerUrl, cb), std::memory_order_relaxed);
        });
        // Let the acceptor bind+listen before the caller connects.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        caller =
            std::make_unique<NativeSrtIngestSession>(/*sourceIndex=*/0, 1920, 1080, &callerRunning);
        IngestCallbacks cb;
        const bool callerOpened = caller->open(callerUrl, cb);
        listenerThread.join();
        ok = callerOpened && listenerOpened.load(std::memory_order_relaxed);
    }
};

} // namespace

class TestSrtIngestTeardown : public QObject {
    Q_OBJECT
private slots:
    void requestStopIsFlagOnly_leavesSocketForCaptureThread();
    void runThenRequestStop_tearsDownCleanly();
};

// The contract that makes the teardown race-free: requestStop(), called from the
// control thread, must NOT close the socket. It only raises the stop flag; the
// capture thread closes its own socket as run() returns. Closing it here is the
// cross-thread srt_close that races the capture thread's srt_recv.
void TestSrtIngestTeardown::requestStopIsFlagOnly_leavesSocketForCaptureThread() {
    LoopbackSrt link(53117);
    QVERIFY2(link.ok, "failed to establish the loopback SRT connection");

    const int connectedHandle = link.caller->m_socket;
    QVERIFY2(connectedHandle != kInvalidSrtSocket, "caller socket should be open after connect");

    link.caller->requestStop();

    QCOMPARE(link.caller->m_socket, connectedHandle); // still open — requestStop is flag-only
}

// The real overlap: stop is requested from the control thread while the capture
// thread is inside run()'s socket-poll loop. The capture thread must observe the
// flag, close its own socket, and return — no crash, no double close.
void TestSrtIngestTeardown::runThenRequestStop_tearsDownCleanly() {
    LoopbackSrt link(53119);
    QVERIFY2(link.ok, "failed to establish the loopback SRT connection");

    std::thread captureThread([&]() { link.caller->run(); });
    // Let run() enter its receive loop before requesting stop.
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    link.caller->requestStop();
    captureThread.join();

    QCOMPARE(link.caller->m_socket, kInvalidSrtSocket); // capture thread closed its own socket
}

QTEST_GUILESS_MAIN(TestSrtIngestTeardown)
#include "tst_srtingest_teardown.moc"
