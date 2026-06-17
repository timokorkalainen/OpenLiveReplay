#ifndef BROADCASTOUTPUTSTATUS_H
#define BROADCASTOUTPUTSTATUS_H

#include "playback/output/broadcastoutputsettings.h"
#include "playback/output/outputdispatcher.h"

#include <QHash>
#include <QString>

namespace BroadcastOutputStatus {

QHash<QString, BroadcastOutputTargetStatus> fromDispatchStats(const OutputDispatchStats& stats);

} // namespace BroadcastOutputStatus

#endif // BROADCASTOUTPUTSTATUS_H
