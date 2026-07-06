#ifndef MEDIAFRAME_H
#define MEDIAFRAME_H

#include "playback/output/outputtypes.h"

#include <QByteArray>
#include <QtGlobal>
#include <limits>

struct MediaAudioFrame {
    int feedIndex = -1;
    qint64 startSample = 0;
    int sampleRate = 48000;
    int channels = 2;
    MediaSampleFormat format = MediaSampleFormat::S16Interleaved;
    QByteArray pcm;

    int sampleFrames() const {
        const int bytesPerFrame = channels * int(sizeof(qint16));
        if (bytesPerFrame <= 0) {
            return 0;
        }
        const qsizetype frames = pcm.size() / bytesPerFrame;
        return static_cast<int>(qMin<qsizetype>(frames, std::numeric_limits<int>::max()));
    }
};

inline QByteArray silentS16Stereo(int sampleFrames) {
    return QByteArray(qMax(0, sampleFrames) * 2 * int(sizeof(qint16)), '\0');
}

#endif // MEDIAFRAME_H
