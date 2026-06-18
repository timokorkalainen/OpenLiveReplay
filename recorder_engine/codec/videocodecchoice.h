#ifndef VIDEOCODECCHOICE_H
#define VIDEOCODECCHOICE_H

#include <QString>

// The recording video codec the user can select.
//   Mpeg2Software — FFmpeg MPEG-2, intra-only, software (the historical path).
//   H264Hardware  — OS hardware H.264 (VideoToolbox / MediaFoundation), intra-only.
enum class VideoCodecChoice { Mpeg2Software, H264Hardware };

inline QString videoCodecToString(VideoCodecChoice codec) {
    return codec == VideoCodecChoice::H264Hardware ? QStringLiteral("h264")
                                                   : QStringLiteral("mpeg2");
}

inline VideoCodecChoice videoCodecFromString(
    const QString& value, VideoCodecChoice fallback = VideoCodecChoice::Mpeg2Software) {
    if (value == QStringLiteral("h264")) return VideoCodecChoice::H264Hardware;
    if (value == QStringLiteral("mpeg2")) return VideoCodecChoice::Mpeg2Software;
    return fallback;
}

#endif // VIDEOCODECCHOICE_H
