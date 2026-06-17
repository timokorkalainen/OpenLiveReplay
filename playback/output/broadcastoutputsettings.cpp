#include "playback/output/broadcastoutputsettings.h"

#include <QVariantMap>

namespace {
constexpr auto kSenderName = "senderName";

QString senderNameKey() {
    return QString::fromLatin1(kSenderName);
}

QList<OutputBusId> orderedBuses(int feedCount) {
    QList<OutputBusId> buses;
    for (int feed = 0; feed < qMax(0, feedCount); ++feed) {
        buses.append(OutputBusId::feed(feed));
    }
    buses.append(OutputBusId::multiview());
    buses.append(OutputBusId::pgm());
    return buses;
}

const OutputTargetAssignment* findAssignment(const QList<OutputTargetAssignment>& outputs,
                                             OutputTargetKind kind, OutputBusId bus) {
    for (const OutputTargetAssignment& assignment : outputs) {
        if (assignment.kind == kind && assignment.sourceBus == bus) return &assignment;
    }
    return nullptr;
}

bool hasCurrentError(const BroadcastOutputTargetStatus& status) {
    return status.runtimeDeadlineMisses > 0 || status.deliveryGaps > 0 ||
           (status.hasLastSubmitResult && !status.lastSubmitSucceeded) ||
           (status.hasLastSinkResult && !status.lastSinkResultSucceeded) ||
           (status.sinkFailures > 0 && status.framesSubmitted == 0);
}

bool hasDegradedHealth(const BroadcastOutputTargetStatus& status) {
    return status.maxQueueDepth > 0 || status.sinkDroppedFrames > 0 ||
           (status.hasLastIdentity && status.lastIdentity.videoPlaceholder);
}

QString statusState(const OutputTargetAssignment& assignment,
                    const BroadcastOutputTargetStatus* status) {
    if (!assignment.enabled) return QStringLiteral("Off");
    if (!status) return QStringLiteral("Waiting");
    if (hasCurrentError(*status)) {
        return QStringLiteral("Error");
    }
    if (hasDegradedHealth(*status)) {
        return QStringLiteral("Degraded");
    }
    if (status->framesSubmitted > 0 || status->sinkSubmittedFrames > 0 ||
        (status->hasLastSubmitResult && status->lastSubmitSucceeded) ||
        (status->hasLastSinkResult && status->lastSinkResultSucceeded)) {
        return QStringLiteral("Active");
    }
    return QStringLiteral("Waiting");
}

QString statusSeverity(const OutputTargetAssignment& assignment,
                       const BroadcastOutputTargetStatus* status) {
    if (!assignment.enabled) return QStringLiteral("off");
    if (!status) return QStringLiteral("warning");
    if (hasCurrentError(*status)) {
        return QStringLiteral("error");
    }
    if (hasDegradedHealth(*status)) {
        return QStringLiteral("warning");
    }
    if (status->framesSubmitted > 0 || status->sinkSubmittedFrames > 0 ||
        (status->hasLastSubmitResult && status->lastSubmitSucceeded) ||
        (status->hasLastSinkResult && status->lastSinkResultSucceeded)) {
        return QStringLiteral("ok");
    }
    return QStringLiteral("warning");
}

QString valueOrDash(qint64 value, bool hasValue) {
    return hasValue ? QString::number(value) : QStringLiteral("-");
}

QString diagnosticText(const OutputTargetAssignment& assignment,
                       const BroadcastOutputTargetStatus* status) {
    if (!assignment.enabled) return QStringLiteral("Output disabled");
    if (!status) return QStringLiteral("Enabled; waiting for output frames");

    const bool hasIdentity = status->hasLastIdentity;
    QString text =
        QStringLiteral(
            "queued=%1 sent=%2 fail=%3 sinkFail=%4 drop=%5 placeholder=%6 silent=%7 repeated=%8 "
            "deadline=%9 cap=%10 q=%11 maxQ=%12 gap=%13 lastQueued=%14 lastDelivered=%15 "
            "sendNs=%16 lastOut=%17 playhead=%18 srcFeed=%19 srcPts=%20")
            .arg(status->framesSubmitted)
            .arg(status->hasSinkStatus ? status->sinkSubmittedFrames : status->framesSubmitted)
            .arg(status->sinkFailures)
            .arg(status->sinkFailedFrames)
            .arg(status->sinkDroppedFrames)
            .arg(status->placeholderFrames)
            .arg(status->silentAudioFrames)
            .arg(status->repeatedPayloadFrames)
            .arg(status->runtimeDeadlineMisses)
            .arg(status->runtimeCatchUpCapHits)
            .arg(status->currentQueueDepth)
            .arg(status->maxQueueDepth)
            .arg(status->deliveryGaps)
            .arg(valueOrDash(status->lastQueuedFrameIndex, status->hasLastQueuedFrameIndex))
            .arg(valueOrDash(status->lastDeliveredFrameIndex, status->hasLastDeliveredFrameIndex))
            .arg(status->lastSubmitDurationNs)
            .arg(valueOrDash(hasIdentity ? status->lastIdentity.outputFrameIndex : 0, hasIdentity))
            .arg(valueOrDash(hasIdentity ? status->lastIdentity.sampledPlayheadMs : 0, hasIdentity))
            .arg(hasIdentity ? QString::number(status->lastIdentity.sourceFeedIndex)
                             : QStringLiteral("-"))
            .arg(valueOrDash(hasIdentity ? status->lastIdentity.sourcePtsMs : 0, hasIdentity));
    if (!status->sinkState.isEmpty()) {
        text += QStringLiteral(" sinkState=%1").arg(status->sinkState);
    }
    if (!status->sinkMessage.isEmpty()) {
        text += QStringLiteral(" message=%1").arg(status->sinkMessage);
    }
    return text;
}

void addStatusFields(QVariantMap& row, const OutputTargetAssignment& assignment,
                     const QHash<QString, BroadcastOutputTargetStatus>& statuses) {
    const auto it = statuses.constFind(assignment.id);
    const BroadcastOutputTargetStatus* status = it == statuses.constEnd() ? nullptr : &it.value();
    const BroadcastOutputTargetStatus empty;
    const BroadcastOutputTargetStatus& values = status ? *status : empty;

    row.insert(QStringLiteral("statusState"), statusState(assignment, status));
    row.insert(QStringLiteral("statusSeverity"), statusSeverity(assignment, status));
    row.insert(QStringLiteral("attemptedFrames"), values.attemptedFrames);
    row.insert(QStringLiteral("framesSubmitted"), values.framesSubmitted);
    row.insert(QStringLiteral("sinkFailures"), values.sinkFailures);
    row.insert(QStringLiteral("sinkSubmittedFrames"), values.sinkSubmittedFrames);
    row.insert(QStringLiteral("sinkFailedFrames"), values.sinkFailedFrames);
    row.insert(QStringLiteral("sinkDroppedFrames"), values.sinkDroppedFrames);
    row.insert(QStringLiteral("currentQueueDepth"), values.currentQueueDepth);
    row.insert(QStringLiteral("maxQueueDepth"), values.maxQueueDepth);
    row.insert(QStringLiteral("deliveryGaps"), values.deliveryGaps);
    row.insert(QStringLiteral("lastQueuedFrameIndex"), values.lastQueuedFrameIndex);
    row.insert(QStringLiteral("lastDeliveredFrameIndex"), values.lastDeliveredFrameIndex);
    row.insert(QStringLiteral("lastSubmitDurationNs"), values.lastSubmitDurationNs);
    row.insert(QStringLiteral("runtimeDeadlineMisses"), values.runtimeDeadlineMisses);
    row.insert(QStringLiteral("runtimeCatchUpCapHits"), values.runtimeCatchUpCapHits);
    row.insert(QStringLiteral("hasSinkStatus"), values.hasSinkStatus);
    row.insert(QStringLiteral("hasLastSubmitResult"), values.hasLastSubmitResult);
    row.insert(QStringLiteral("lastSubmitSucceeded"), values.lastSubmitSucceeded);
    row.insert(QStringLiteral("hasLastSinkResult"), values.hasLastSinkResult);
    row.insert(QStringLiteral("lastSinkResultSucceeded"), values.lastSinkResultSucceeded);
    row.insert(QStringLiteral("hasLastQueuedFrameIndex"), values.hasLastQueuedFrameIndex);
    row.insert(QStringLiteral("hasLastDeliveredFrameIndex"), values.hasLastDeliveredFrameIndex);
    row.insert(QStringLiteral("sinkState"), values.sinkState);
    row.insert(QStringLiteral("sinkMessage"), values.sinkMessage);
    row.insert(QStringLiteral("placeholderFrames"), values.placeholderFrames);
    row.insert(QStringLiteral("silentAudioFrames"), values.silentAudioFrames);
    row.insert(QStringLiteral("repeatedPayloadFrames"), values.repeatedPayloadFrames);
    row.insert(QStringLiteral("lastOutputFrameIndex"),
               values.hasLastIdentity ? values.lastIdentity.outputFrameIndex : -1);
    row.insert(QStringLiteral("lastPlayheadMs"),
               values.hasLastIdentity ? values.lastIdentity.sampledPlayheadMs : -1);
    row.insert(QStringLiteral("lastSourceFeedIndex"),
               values.hasLastIdentity ? values.lastIdentity.sourceFeedIndex : -1);
    row.insert(QStringLiteral("lastSourcePtsMs"),
               values.hasLastIdentity ? values.lastIdentity.sourcePtsMs : -1);
    row.insert(QStringLiteral("lastVideoPlaceholder"),
               values.hasLastIdentity && values.lastIdentity.videoPlaceholder);
    row.insert(QStringLiteral("lastAudioSilent"),
               !values.hasLastIdentity || values.lastIdentity.audioSilent);
    row.insert(QStringLiteral("diagnostic"), diagnosticText(assignment, status));
}
} // namespace

namespace BroadcastOutputSettings {

QList<OutputTargetAssignment> ensureTargets(const QList<OutputTargetAssignment>& outputs,
                                            int feedCount, OutputTargetKind kind) {
    QList<OutputTargetAssignment> result;
    for (const OutputTargetAssignment& assignment : outputs) {
        if (assignment.kind != kind) result.append(assignment);
    }

    for (OutputBusId bus : orderedBuses(feedCount)) {
        OutputTargetAssignment assignment;
        if (const OutputTargetAssignment* existing = findAssignment(outputs, kind, bus)) {
            assignment = *existing;
        }
        assignment.id = targetId(bus, kind);
        assignment.sourceBus = bus;
        assignment.kind = kind;
        if (!assignment.settings.contains(senderNameKey()) ||
            assignment.settings.value(senderNameKey()).toString().trimmed().isEmpty()) {
            assignment.settings.insert(senderNameKey(), defaultSenderName(bus));
        }
        result.append(assignment);
    }
    return result;
}

QList<OutputTargetAssignment> setEnabled(const QList<OutputTargetAssignment>& outputs,
                                         int feedCount, OutputTargetKind kind, OutputBusId bus,
                                         bool enabled) {
    QList<OutputTargetAssignment> result = ensureTargets(outputs, feedCount, kind);
    for (OutputTargetAssignment& assignment : result) {
        if (assignment.kind == kind && assignment.sourceBus == bus) {
            assignment.enabled = enabled;
            break;
        }
    }
    return result;
}

QList<OutputTargetAssignment> setSenderName(const QList<OutputTargetAssignment>& outputs,
                                            int feedCount, OutputTargetKind kind, OutputBusId bus,
                                            const QString& senderName) {
    QList<OutputTargetAssignment> result = ensureTargets(outputs, feedCount, kind);
    for (OutputTargetAssignment& assignment : result) {
        if (assignment.kind == kind && assignment.sourceBus == bus) {
            const QString normalized = senderName.trimmed();
            assignment.settings.insert(senderNameKey(),
                                       normalized.isEmpty() ? defaultSenderName(bus) : normalized);
            break;
        }
    }
    return result;
}

bool isEnabled(const QList<OutputTargetAssignment>& outputs, OutputTargetKind kind,
               OutputBusId bus) {
    const OutputTargetAssignment* assignment = findAssignment(outputs, kind, bus);
    return assignment && assignment->enabled;
}

QString senderName(const QList<OutputTargetAssignment>& outputs, OutputTargetKind kind,
                   OutputBusId bus) {
    const OutputTargetAssignment* assignment = findAssignment(outputs, kind, bus);
    if (!assignment) return defaultSenderName(bus);
    const QString name = assignment->settings.value(senderNameKey()).toString().trimmed();
    return name.isEmpty() ? defaultSenderName(bus) : name;
}

QVariantList rows(const QList<OutputTargetAssignment>& outputs, int feedCount,
                  OutputTargetKind kind) {
    return rows(outputs, feedCount, kind, {});
}

QVariantList rows(const QList<OutputTargetAssignment>& outputs, int feedCount,
                  OutputTargetKind kind,
                  const QHash<QString, BroadcastOutputTargetStatus>& statuses) {
    const QList<OutputTargetAssignment> normalized = ensureTargets(outputs, feedCount, kind);

    QVariantList list;
    for (OutputBusId bus : orderedBuses(feedCount)) {
        const OutputTargetAssignment* assignment = findAssignment(normalized, kind, bus);
        if (!assignment) continue;

        QVariantMap row;
        row.insert(QStringLiteral("id"), assignment->id);
        row.insert(QStringLiteral("kind"), outputTargetKindName(kind));
        row.insert(QStringLiteral("busKind"), busKindName(bus));
        row.insert(QStringLiteral("feedIndex"), bus.kind == OutputBusKind::Feed ? bus.index : -1);
        row.insert(QStringLiteral("label"), label(bus));
        row.insert(QStringLiteral("enabled"), assignment->enabled);
        row.insert(QStringLiteral("senderName"), senderName(normalized, kind, bus));
        addStatusFields(row, *assignment, statuses);
        list.append(row);
    }
    return list;
}

QList<OutputTargetAssignment> qtPreviewAssignments(int feedCount, bool includeMultiview,
                                                   bool includePgm) {
    QList<OutputTargetAssignment> assignments;
    for (int feed = 0; feed < qMax(0, feedCount); ++feed) {
        OutputTargetAssignment assignment;
        assignment.id = QStringLiteral("qt-preview-feed-%1").arg(feed);
        assignment.sourceBus = OutputBusId::feed(feed);
        assignment.kind = OutputTargetKind::QtPreview;
        assignment.enabled = true;
        assignments.append(assignment);
    }
    if (includeMultiview) {
        OutputTargetAssignment assignment;
        assignment.id = QStringLiteral("qt-preview-multiview");
        assignment.sourceBus = OutputBusId::multiview();
        assignment.kind = OutputTargetKind::QtPreview;
        assignment.enabled = true;
        assignments.append(assignment);
    }
    if (includePgm) {
        OutputTargetAssignment assignment;
        assignment.id = QStringLiteral("qt-preview-pgm");
        assignment.sourceBus = OutputBusId::pgm();
        assignment.kind = OutputTargetKind::QtPreview;
        assignment.enabled = true;
        assignments.append(assignment);
    }
    return assignments;
}

OutputBusId busFromUiKey(const QString& busKind, int feedIndex) {
    if (busKind == QStringLiteral("feed")) return OutputBusId::feed(qMax(0, feedIndex));
    if (busKind == QStringLiteral("multiview")) return OutputBusId::multiview();
    return OutputBusId::pgm();
}

QString busKindName(OutputBusId bus) {
    switch (bus.kind) {
    case OutputBusKind::Feed:
        return QStringLiteral("feed");
    case OutputBusKind::Multiview:
        return QStringLiteral("multiview");
    case OutputBusKind::Pgm:
        return QStringLiteral("pgm");
    }
    return QStringLiteral("pgm");
}

QString label(OutputBusId bus) {
    switch (bus.kind) {
    case OutputBusKind::Feed:
        return QStringLiteral("Feed %1").arg(bus.index + 1);
    case OutputBusKind::Multiview:
        return QStringLiteral("Multiview");
    case OutputBusKind::Pgm:
        return QStringLiteral("PGM");
    }
    return QStringLiteral("PGM");
}

QString defaultSenderName(OutputBusId bus) {
    return QStringLiteral("OpenLiveReplay %1").arg(label(bus));
}

QString targetId(OutputBusId bus, OutputTargetKind kind) {
    const QString suffix = outputTargetKindName(kind);
    switch (bus.kind) {
    case OutputBusKind::Feed:
        return QStringLiteral("feed%1-%2").arg(bus.index).arg(suffix);
    case OutputBusKind::Multiview:
        return QStringLiteral("multiview-%1").arg(suffix);
    case OutputBusKind::Pgm:
        return QStringLiteral("pgm-%1").arg(suffix);
    }
    return QStringLiteral("pgm-%1").arg(suffix);
}

} // namespace BroadcastOutputSettings
