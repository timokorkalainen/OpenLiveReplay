#ifndef MEDIAFOUNDATIONASYNCLIFECYCLE_H
#define MEDIAFOUNDATIONASYNCLIFECYCLE_H

enum class MfAsyncMessage {
    Flush,
    EndOfStream,
    Drain,
    EndStreaming,
};

enum class MfAsyncPollResult {
    Continue,
    DrainComplete,
    Failed,
    TimedOut,
};

enum class MfAsyncFinalizeResult {
    Complete,
    MessageFailed,
    EventFailed,
    TimedOut,
    ShutdownFailed,
};

struct MfAsyncAbortResult {
    bool flushSucceeded = false;
    bool endStreamingSucceeded = false;
    bool shutdownSucceeded = false;
};

template <typename ProcessMessage, typename Shutdown>
MfAsyncAbortResult abortMfAsyncTransform(ProcessMessage processMessage, Shutdown shutdown) {
    MfAsyncAbortResult result;
    result.flushSucceeded = processMessage(MfAsyncMessage::Flush);
    result.endStreamingSucceeded = processMessage(MfAsyncMessage::EndStreaming);
    result.shutdownSucceeded = shutdown();
    return result;
}

template <typename ProcessMessage, typename PollEvent, typename Shutdown>
MfAsyncFinalizeResult finalizeMfAsyncTransform(ProcessMessage processMessage, PollEvent pollEvent,
                                               Shutdown shutdown, int maxPolls = 100000) {
    if (!processMessage(MfAsyncMessage::EndOfStream) || !processMessage(MfAsyncMessage::Drain)) {
        return MfAsyncFinalizeResult::MessageFailed;
    }

    for (int poll = 0; poll < maxPolls; ++poll) {
        switch (pollEvent()) {
        case MfAsyncPollResult::Continue:
            break;
        case MfAsyncPollResult::DrainComplete: {
            const bool endStreamingSucceeded = processMessage(MfAsyncMessage::EndStreaming);
            const bool shutdownSucceeded = shutdown();
            if (!endStreamingSucceeded) {
                return MfAsyncFinalizeResult::MessageFailed;
            }
            return shutdownSucceeded ? MfAsyncFinalizeResult::Complete
                                     : MfAsyncFinalizeResult::ShutdownFailed;
        }
        case MfAsyncPollResult::Failed:
            return MfAsyncFinalizeResult::EventFailed;
        case MfAsyncPollResult::TimedOut:
            return MfAsyncFinalizeResult::TimedOut;
        }
    }
    return MfAsyncFinalizeResult::TimedOut;
}

#endif // MEDIAFOUNDATIONASYNCLIFECYCLE_H
