#ifndef FRAMEPIXELFORMAT_H
#define FRAMEPIXELFORMAT_H

enum class FramePixelFormat {
    Nv12,
    Yuv420p,
    Rgba8,
};

constexpr int planeCount(FramePixelFormat format) {
    switch (format) {
    case FramePixelFormat::Nv12:
        return 2;
    case FramePixelFormat::Yuv420p:
        return 3;
    case FramePixelFormat::Rgba8:
        return 1;
    }
    return 0;
}

#endif // FRAMEPIXELFORMAT_H
