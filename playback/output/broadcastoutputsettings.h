#ifndef BROADCASTOUTPUTSETTINGS_H
#define BROADCASTOUTPUTSETTINGS_H

#include "playback/output/outputbusengine.h"
#include "playback/output/outputtargetassignment.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QVariantList>

struct BroadcastOutputTargetStatus {
    qint64 attemptedFrames = 0;
    qint64 framesSubmitted = 0;
    qint64 sinkFailures = 0;
    qint64 sinkSubmittedFrames = 0;
    qint64 sinkFailedFrames = 0;
    qint64 sinkDroppedFrames = 0;
    qint64 currentQueueDepth = 0;
    qint64 maxQueueDepth = 0;
    qint64 deliveryGaps = 0;
    qint64 lastQueuedFrameIndex = -1;
    qint64 lastDeliveredFrameIndex = -1;
    qint64 lastSubmitDurationNs = 0;
    qint64 runtimeDeadlineMisses = 0;
    qint64 runtimeCatchUpCapHits = 0;
    qint64 placeholderFrames = 0;
    qint64 silentAudioFrames = 0;
    qint64 repeatedPayloadFrames = 0;
    bool hasSinkStatus = false;
    bool hasLastSubmitResult = false;
    bool lastSubmitSucceeded = true;
    bool hasLastSinkResult = false;
    bool lastSinkResultSucceeded = true;
    bool hasLastQueuedFrameIndex = false;
    bool hasLastDeliveredFrameIndex = false;
    QString sinkState;
    QString sinkMessage;
    bool hasLastIdentity = false;
    OutputFrameIdentity lastIdentity;
};

namespace BroadcastOutputSettings {

QList<OutputTargetAssignment> ensureTargets(const QList<OutputTargetAssignment>& outputs,
                                            int feedCount, OutputTargetKind kind);
QList<OutputTargetAssignment> setEnabled(const QList<OutputTargetAssignment>& outputs,
                                         int feedCount, OutputTargetKind kind, OutputBusId bus,
                                         bool enabled);
QList<OutputTargetAssignment> setSenderName(const QList<OutputTargetAssignment>& outputs,
                                            int feedCount, OutputTargetKind kind, OutputBusId bus,
                                            const QString& senderName);

bool isEnabled(const QList<OutputTargetAssignment>& outputs, OutputTargetKind kind,
               OutputBusId bus);
QString senderName(const QList<OutputTargetAssignment>& outputs, OutputTargetKind kind,
                   OutputBusId bus);
QVariantList rows(const QList<OutputTargetAssignment>& outputs, int feedCount,
                  OutputTargetKind kind);
QVariantList rows(const QList<OutputTargetAssignment>& outputs, int feedCount,
                  OutputTargetKind kind,
                  const QHash<QString, BroadcastOutputTargetStatus>& statuses);
QList<OutputTargetAssignment> qtPreviewAssignments(int feedCount, bool includeMultiview,
                                                   bool includePgm);

OutputBusId busFromUiKey(const QString& busKind, int feedIndex);
QString busKindName(OutputBusId bus);
QString label(OutputBusId bus);
QString defaultSenderName(OutputBusId bus);
QString targetId(OutputBusId bus, OutputTargetKind kind);

} // namespace BroadcastOutputSettings

#endif // BROADCASTOUTPUTSETTINGS_H
