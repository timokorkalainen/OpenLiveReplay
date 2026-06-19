#ifndef OLR_RECORDGATE_H
#define OLR_RECORDGATE_H

#include "recorder_engine/codec/videocodecchoice.h"

#include <QString>

// True only when H.264 is selected but no hardware encoder exists (hard block).
bool recordCodecUnavailable(VideoCodecChoice codec, bool h264HardwareAvailable);

// True only when a positive benchmarked safe count is exceeded (soft warn).
// safeFeeds <= 0 means "not benchmarked / unknown" and never warns.
bool feedCountExceedsSafe(int configuredFeeds, int safeFeeds);

// Actionable message for the hard-block case.
QString recordCodecBlockReason(VideoCodecChoice codec);

#endif // OLR_RECORDGATE_H
