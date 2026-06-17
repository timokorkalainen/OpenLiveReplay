#include "playback/output/broadcastoutputstatus.h"

namespace BroadcastOutputStatus {

QHash<QString, BroadcastOutputTargetStatus> fromDispatchStats(const OutputDispatchStats& stats) {
    QHash<QString, BroadcastOutputTargetStatus> statuses;
    for (auto it = stats.targets.constBegin(); it != stats.targets.constEnd(); ++it) {
        const OutputTargetDispatchStats& source = it.value();
        BroadcastOutputTargetStatus status;
        status.attemptedFrames = source.attemptedFrames;
        status.framesSubmitted = source.framesSubmitted;
        status.sinkFailures = source.sinkFailures;
        status.sinkSubmittedFrames = source.sinkSubmittedFrames;
        status.sinkFailedFrames = source.sinkFailedFrames;
        status.sinkDroppedFrames = source.sinkDroppedFrames;
        status.placeholderFrames = source.placeholderFrames;
        status.silentAudioFrames = source.silentAudioFrames;
        status.repeatedPayloadFrames = source.repeatedPayloadFrames;
        status.hasSinkStatus = source.hasSinkStatus;
        status.hasLastSubmitResult = source.hasLastSubmitResult;
        status.lastSubmitSucceeded = source.lastSubmitSucceeded;
        status.hasLastSinkResult = source.hasLastSinkResult;
        status.lastSinkResultSucceeded = source.lastSinkResultSucceeded;
        status.sinkState = source.sinkState;
        status.sinkMessage = source.sinkMessage;
        status.hasLastIdentity = source.hasLastIdentity;
        status.lastIdentity = source.lastIdentity;
        statuses.insert(it.key(), status);
    }
    return statuses;
}

} // namespace BroadcastOutputStatus
