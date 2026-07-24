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

enum class MfAsyncShutdownStatus {
    Initiated,
    Completed,
    Failed,
};

enum class MfAsyncShutdownResult {
    Complete,
    ShutdownFailed,
    StatusFailed,
    TimedOut,
};

enum class MfAsyncResourceDisposition {
    Release,
    Retain,
};

struct MfAsyncAbortResult {
    bool flushSucceeded = false;
    bool endStreamingSucceeded = false;
    bool shutdownSucceeded = false;
};

constexpr MfAsyncResourceDisposition
mfAsyncResourceDisposition(MfAsyncShutdownResult result) noexcept {
    return result == MfAsyncShutdownResult::Complete ? MfAsyncResourceDisposition::Release
                                                     : MfAsyncResourceDisposition::Retain;
}

template <typename... RetainResource>
void retainMfAsyncResourcesIfIncomplete(MfAsyncShutdownResult result,
                                        RetainResource... retainResource) {
    if (mfAsyncResourceDisposition(result) == MfAsyncResourceDisposition::Release) {
        return;
    }
    (retainResource(), ...);
}

template <typename Shutdown, typename PollStatus, typename DeadlineExpired, typename Wait>
MfAsyncShutdownResult completeMfAsyncShutdown(Shutdown shutdown, PollStatus pollStatus,
                                              DeadlineExpired deadlineExpired, Wait wait) {
    if (!shutdown()) {
        return MfAsyncShutdownResult::ShutdownFailed;
    }

    while (!deadlineExpired()) {
        switch (pollStatus()) {
        case MfAsyncShutdownStatus::Completed:
            return MfAsyncShutdownResult::Complete;
        case MfAsyncShutdownStatus::Failed:
            return MfAsyncShutdownResult::StatusFailed;
        case MfAsyncShutdownStatus::Initiated:
            wait();
            break;
        }
    }
    return MfAsyncShutdownResult::TimedOut;
}

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
