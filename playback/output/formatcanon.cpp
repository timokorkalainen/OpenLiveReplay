#include "playback/output/formatcanon.h"

namespace formatcanon {
namespace {

int chromaWidth(int frameWidth) {
    return (frameWidth + 1) / 2;
}

int chromaHeight(int frameHeight) {
    return (frameHeight + 1) / 2;
}

qsizetype planeBytes(int stride, int rows) {
    return static_cast<qsizetype>(stride) * static_cast<qsizetype>(rows);
}

qsizetype byteOffset(int row, int stride) {
    return static_cast<qsizetype>(row) * static_cast<qsizetype>(stride);
}

uchar clampU8(int value) {
    if (value < 0) return 0;
    if (value > 255) return 255;
    return static_cast<uchar>(value);
}

} // namespace

PlaneShape planeShape(FramePixelFormat format, int frameWidth, int frameHeight, int planeIndex) {
    if (planeIndex < 0 || planeIndex >= planeCount(format)) return {};

    switch (format) {
    case FramePixelFormat::Yuv420p:
        if (planeIndex == 0) return {frameWidth, frameHeight};
        return {chromaWidth(frameWidth), chromaHeight(frameHeight)};
    case FramePixelFormat::Nv12:
        if (planeIndex == 0) return {frameWidth, frameHeight};
        return {2 * chromaWidth(frameWidth), chromaHeight(frameHeight)};
    case FramePixelFormat::Rgba8:
        return {frameWidth, frameHeight};
    }
    return {};
}

int bytesPerSample(FramePixelFormat format, int planeIndex) {
    if (format == FramePixelFormat::Rgba8 && planeIndex == 0) return 4;
    return 1;
}

int packedStride(FramePixelFormat format, int frameWidth, int frameHeight, int planeIndex) {
    const PlaneShape shape = planeShape(format, frameWidth, frameHeight, planeIndex);
    return shape.width * bytesPerSample(format, planeIndex);
}

CpuPlanes nv12ToYuv420p(const CpuPlanes& src) {
    if (src.format != FramePixelFormat::Nv12 || !src.isValid()) return {};

    const int w = src.width;
    const int h = src.height;
    const int cw = chromaWidth(w);
    const int ch = chromaHeight(h);

    CpuPlanes out;
    out.format = FramePixelFormat::Yuv420p;
    out.width = w;
    out.height = h;
    out.stride[0] = w;
    out.stride[1] = cw;
    out.stride[2] = cw;
    out.plane[0] = QByteArray(planeBytes(w, h), 0);
    out.plane[1] = QByteArray(planeBytes(cw, ch), 0);
    out.plane[2] = QByteArray(planeBytes(cw, ch), 0);

    for (int y = 0; y < h; ++y) {
        const char* srcRow = src.plane[0].constData() + byteOffset(y, src.stride[0]);
        char* dstRow = out.plane[0].data() + byteOffset(y, out.stride[0]);
        for (int x = 0; x < w; ++x)
            dstRow[x] = srcRow[x];
    }
    for (int y = 0; y < ch; ++y) {
        const char* srcRow = src.plane[1].constData() + byteOffset(y, src.stride[1]);
        char* uRow = out.plane[1].data() + byteOffset(y, out.stride[1]);
        char* vRow = out.plane[2].data() + byteOffset(y, out.stride[2]);
        for (int x = 0; x < cw; ++x) {
            const qsizetype srcX = static_cast<qsizetype>(2) * x;
            uRow[x] = srcRow[srcX];
            vRow[x] = srcRow[srcX + 1];
        }
    }
    return out;
}

CpuPlanes yuv420pToNv12(const CpuPlanes& src) {
    if (src.format != FramePixelFormat::Yuv420p || !src.isValid()) return {};

    const int w = src.width;
    const int h = src.height;
    const int cw = chromaWidth(w);
    const int ch = chromaHeight(h);

    CpuPlanes out;
    out.format = FramePixelFormat::Nv12;
    out.width = w;
    out.height = h;
    out.stride[0] = w;
    out.stride[1] = 2 * cw;
    out.plane[0] = QByteArray(planeBytes(w, h), 0);
    out.plane[1] = QByteArray(planeBytes(out.stride[1], ch), 0);

    for (int y = 0; y < h; ++y) {
        const char* srcRow = src.plane[0].constData() + byteOffset(y, src.stride[0]);
        char* dstRow = out.plane[0].data() + byteOffset(y, out.stride[0]);
        for (int x = 0; x < w; ++x)
            dstRow[x] = srcRow[x];
    }
    for (int y = 0; y < ch; ++y) {
        const char* uRow = src.plane[1].constData() + byteOffset(y, src.stride[1]);
        const char* vRow = src.plane[2].constData() + byteOffset(y, src.stride[2]);
        char* dstRow = out.plane[1].data() + byteOffset(y, out.stride[1]);
        for (int x = 0; x < cw; ++x) {
            const qsizetype dstX = static_cast<qsizetype>(2) * x;
            dstRow[dstX] = uRow[x];
            dstRow[dstX + 1] = vRow[x];
        }
    }
    return out;
}

FullResChroma upsampleChromaNearest(const CpuPlanes& yuv420p) {
    if (yuv420p.format != FramePixelFormat::Yuv420p || !yuv420p.isValid()) return {};

    const int w = yuv420p.width;
    const int h = yuv420p.height;
    const int cw = chromaWidth(w);
    const int ch = chromaHeight(h);

    FullResChroma out;
    out.width = w;
    out.height = h;
    out.u = QByteArray(planeBytes(w, h), 0);
    out.v = QByteArray(planeBytes(w, h), 0);

    for (int y = 0; y < h; ++y) {
        const int cy = (y >> 1) < ch ? (y >> 1) : (ch - 1);
        const char* uRow = yuv420p.plane[1].constData() + byteOffset(cy, yuv420p.stride[1]);
        const char* vRow = yuv420p.plane[2].constData() + byteOffset(cy, yuv420p.stride[2]);
        char* uDst = out.u.data() + byteOffset(y, w);
        char* vDst = out.v.data() + byteOffset(y, w);
        for (int x = 0; x < w; ++x) {
            const int cx = (x >> 1) < cw ? (x >> 1) : (cw - 1);
            uDst[x] = uRow[cx];
            vDst[x] = vRow[cx];
        }
    }
    return out;
}

Rgb8 yuvToRgb8(uchar y, uchar u, uchar v, ColorMatrix matrix, ColorRange range) {
    const int yp = (range == ColorRange::Video) ? (static_cast<int>(y) - 16) * 1192
                                                : static_cast<int>(y) * 1024;
    const int cb = static_cast<int>(u) - 128;
    const int cr = static_cast<int>(v) - 128;

    int r = 0;
    int g = 0;
    int b = 0;
    if (matrix == ColorMatrix::Bt601) {
        r = yp + 1634 * cr;
        g = yp - 401 * cb - 832 * cr;
        b = yp + 2066 * cb;
    } else {
        r = yp + 1836 * cr;
        g = yp - 218 * cb - 546 * cr;
        b = yp + 2164 * cb;
    }

    return {clampU8((r + 512) >> 10), clampU8((g + 512) >> 10), clampU8((b + 512) >> 10)};
}

} // namespace formatcanon
