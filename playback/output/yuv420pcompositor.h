#ifndef YUV420PCOMPOSITOR_H
#define YUV420PCOMPOSITOR_H

#include "playback/output/framehandle.h"

#include <QList>

class Yuv420pCompositor {
public:
    static FrameHandle composeGrid(const QList<FrameHandle>& frames, int width, int height);
};

#endif // YUV420PCOMPOSITOR_H
