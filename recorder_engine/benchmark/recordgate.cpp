#include "recorder_engine/benchmark/recordgate.h"

bool recordCodecUnavailable(VideoCodecChoice codec, bool h264HardwareAvailable) {
    return codec == VideoCodecChoice::H264Hardware && !h264HardwareAvailable;
}

bool feedCountExceedsSafe(int configuredFeeds, int safeFeeds) {
    return safeFeeds > 0 && configuredFeeds > safeFeeds;
}

QString recordCodecBlockReason(VideoCodecChoice codec) {
    if (codec == VideoCodecChoice::H264Hardware) {
        return QStringLiteral("H.264 hardware encoding is not available on this device. "
                              "Select MPEG-2 (software) to record.");
    }
    return QString();
}
