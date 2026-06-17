#ifndef MEDIAFRAME_H
#define MEDIAFRAME_H

#include "playback/output/outputtypes.h"

#include <QByteArray>
#include <QtGlobal>

struct MediaVideoFrame {
    int feedIndex = -1;
    qint64 ptsMs = 0;
    qint64 outputFrameIndex = -1;
    int width = 0;
    int height = 0;
    MediaPixelFormat format = MediaPixelFormat::Invalid;
    QByteArray planeY;
    QByteArray planeU;
    QByteArray planeV;
    int strideY = 0;
    int strideU = 0;
    int strideV = 0;
    bool isPlaceholder = false;

    bool isValid() const {
        return width > 0 && height > 0 && format == MediaPixelFormat::Yuv420p &&
               !planeY.isEmpty() && !planeU.isEmpty() && !planeV.isEmpty();
    }

    static MediaVideoFrame solidYuv420p(int width, int height, uchar y, uchar u, uchar v) {
        MediaVideoFrame f;
        f.width = width;
        f.height = height;
        f.format = MediaPixelFormat::Yuv420p;
        f.strideY = width;
        f.strideU = (width + 1) / 2;
        f.strideV = (width + 1) / 2;
        const int chromaH = (height + 1) / 2;
        f.planeY = QByteArray(width * height, char(y));
        f.planeU = QByteArray(f.strideU * chromaH, char(u));
        f.planeV = QByteArray(f.strideV * chromaH, char(v));
        return f;
    }
};

struct MediaAudioFrame {
    int feedIndex = -1;
    qint64 startSample = 0;
    int sampleRate = 48000;
    int channels = 2;
    MediaSampleFormat format = MediaSampleFormat::S16Interleaved;
    QByteArray pcm;

    int sampleFrames() const {
        const int bytesPerFrame = channels * int(sizeof(qint16));
        return bytesPerFrame > 0 ? pcm.size() / bytesPerFrame : 0;
    }
};

inline QByteArray silentS16Stereo(int sampleFrames) {
    return QByteArray(qMax(0, sampleFrames) * 2 * int(sizeof(qint16)), '\0');
}

#endif // MEDIAFRAME_H
