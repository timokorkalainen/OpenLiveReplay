#ifndef YUV420PCOMPOSITOR_H
#define YUV420PCOMPOSITOR_H

#include "playback/output/mediaframe.h"

#include <QList>

class Yuv420pCompositor {
public:
    static MediaVideoFrame composeGrid(const QList<MediaVideoFrame>& frames, int width, int height);
};

#endif // YUV420PCOMPOSITOR_H
