#ifndef BROADCASTOUTPUTSETTINGS_H
#define BROADCASTOUTPUTSETTINGS_H

#include "playback/output/outputtargetassignment.h"

#include <QList>
#include <QString>
#include <QVariantList>

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

OutputBusId busFromUiKey(const QString& busKind, int feedIndex);
QString busKindName(OutputBusId bus);
QString label(OutputBusId bus);
QString defaultSenderName(OutputBusId bus);
QString targetId(OutputBusId bus, OutputTargetKind kind);

} // namespace BroadcastOutputSettings

#endif // BROADCASTOUTPUTSETTINGS_H
