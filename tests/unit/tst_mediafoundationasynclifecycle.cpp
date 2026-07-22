#include <QtTest>

#include <vector>

#include "recorder_engine/codec/mediafoundationasynclifecycle.h"

class TestMediaFoundationAsyncLifecycle : public QObject {
    Q_OBJECT
private slots:
    void abortFlushesAndShutsDownWithoutDrain();
    void abortAttemptsEveryStepAfterFailures();
    void finalizeDrainsOutputToCompletion();
    void finalizeStopsOnMessageFailure();
    void finalizeReportsEventFailureAndTimeout();
};

void TestMediaFoundationAsyncLifecycle::abortFlushesAndShutsDownWithoutDrain() {
    std::vector<MfAsyncMessage> messages;
    int shutdownCalls = 0;

    const MfAsyncAbortResult result = abortMfAsyncTransform(
        [&](MfAsyncMessage message) {
            messages.push_back(message);
            return true;
        },
        [&] {
            ++shutdownCalls;
            return true;
        });

    QCOMPARE(messages,
             (std::vector<MfAsyncMessage>{MfAsyncMessage::Flush, MfAsyncMessage::EndStreaming}));
    QCOMPARE(shutdownCalls, 1);
    QVERIFY(result.flushSucceeded);
    QVERIFY(result.endStreamingSucceeded);
    QVERIFY(result.shutdownSucceeded);
}

void TestMediaFoundationAsyncLifecycle::abortAttemptsEveryStepAfterFailures() {
    std::vector<QString> calls;

    const MfAsyncAbortResult result = abortMfAsyncTransform(
        [&](MfAsyncMessage message) {
            calls.push_back(message == MfAsyncMessage::Flush ? QStringLiteral("flush")
                                                             : QStringLiteral("end-streaming"));
            return false;
        },
        [&] {
            calls.push_back(QStringLiteral("shutdown"));
            return false; // Models IMFShutdown QI or Shutdown failure.
        });

    QCOMPARE(calls, (std::vector<QString>{QStringLiteral("flush"), QStringLiteral("end-streaming"),
                                          QStringLiteral("shutdown")}));
    QVERIFY(!result.flushSucceeded);
    QVERIFY(!result.endStreamingSucceeded);
    QVERIFY(!result.shutdownSucceeded);
}

void TestMediaFoundationAsyncLifecycle::finalizeDrainsOutputToCompletion() {
    std::vector<MfAsyncMessage> messages;
    int polls = 0;
    int outputs = 0;
    int shutdownCalls = 0;

    const MfAsyncFinalizeResult result = finalizeMfAsyncTransform(
        [&](MfAsyncMessage message) {
            messages.push_back(message);
            return true;
        },
        [&] {
            ++polls;
            if (polls <= 2) {
                ++outputs;
                return MfAsyncPollResult::Continue;
            }
            return MfAsyncPollResult::DrainComplete;
        },
        [&] {
            ++shutdownCalls;
            return true;
        });

    QCOMPARE(messages,
             (std::vector<MfAsyncMessage>{MfAsyncMessage::EndOfStream, MfAsyncMessage::Drain,
                                          MfAsyncMessage::EndStreaming}));
    QCOMPARE(outputs, 2);
    QCOMPARE(shutdownCalls, 1);
    QCOMPARE(result, MfAsyncFinalizeResult::Complete);
}

void TestMediaFoundationAsyncLifecycle::finalizeStopsOnMessageFailure() {
    int messageCalls = 0;
    int pollCalls = 0;

    const MfAsyncFinalizeResult result = finalizeMfAsyncTransform(
        [&](MfAsyncMessage) {
            ++messageCalls;
            return messageCalls == 1;
        },
        [&] {
            ++pollCalls;
            return MfAsyncPollResult::DrainComplete;
        },
        [] { return true; });

    QCOMPARE(messageCalls, 2);
    QCOMPARE(pollCalls, 0);
    QCOMPARE(result, MfAsyncFinalizeResult::MessageFailed);
}

void TestMediaFoundationAsyncLifecycle::finalizeReportsEventFailureAndTimeout() {
    auto processMessage = [](MfAsyncMessage) { return true; };
    QCOMPARE(finalizeMfAsyncTransform(
                 processMessage, [] { return MfAsyncPollResult::Failed; }, [] { return true; }),
             MfAsyncFinalizeResult::EventFailed);

    int timeoutPolls = 0;
    QCOMPARE(finalizeMfAsyncTransform(
                 processMessage,
                 [&] {
                     ++timeoutPolls;
                     return MfAsyncPollResult::TimedOut;
                 },
                 [] { return true; }),
             MfAsyncFinalizeResult::TimedOut);
    QCOMPARE(timeoutPolls, 1);

    int boundedPolls = 0;
    QCOMPARE(finalizeMfAsyncTransform(
                 processMessage,
                 [&] {
                     ++boundedPolls;
                     return MfAsyncPollResult::Continue;
                 },
                 [] { return true; }, 3),
             MfAsyncFinalizeResult::TimedOut);
    QCOMPARE(boundedPolls, 3);
}

QTEST_GUILESS_MAIN(TestMediaFoundationAsyncLifecycle)
#include "tst_mediafoundationasynclifecycle.moc"
