#include "playback/output/framehandle.h"

#include <algorithm>
#include <utility>

IFrameData::~IFrameData() = default;

namespace {

qsizetype planeBytes(int stride, int rows) {
    return static_cast<qsizetype>(stride) * static_cast<qsizetype>(rows);
}

qsizetype byteOffset(int row, int stride) {
    return static_cast<qsizetype>(row) * static_cast<qsizetype>(stride);
}

CpuPlanes convertYuv420pToNv12(const CpuPlanes& in) {
    CpuPlanes out;
    out.format = FramePixelFormat::Nv12;
    out.width = in.width;
    out.height = in.height;
    const int chromaW = (in.width + 1) / 2;
    const int chromaH = (in.height + 1) / 2;
    out.stride[0] = in.stride[0] > 0 ? in.stride[0] : in.width;
    out.stride[1] = chromaW * 2;
    out.plane[0] = in.plane[0];
    out.plane[1] = QByteArray(planeBytes(out.stride[1], chromaH), '\0');

    char* dst = out.plane[1].data();
    const char* u = in.plane[1].constData();
    const char* v = in.plane[2].constData();
    for (int row = 0; row < chromaH; ++row) {
        char* d = dst + byteOffset(row, out.stride[1]);
        const char* us = u + byteOffset(row, in.stride[1]);
        const char* vs = v + byteOffset(row, in.stride[2]);
        for (int x = 0; x < chromaW; ++x) {
            const qsizetype dx = static_cast<qsizetype>(2) * x;
            d[dx] = us[x];
            d[dx + 1] = vs[x];
        }
    }
    return out;
}

CpuPlanes convertNv12ToYuv420p(const CpuPlanes& in) {
    CpuPlanes out;
    out.format = FramePixelFormat::Yuv420p;
    out.width = in.width;
    out.height = in.height;
    const int chromaW = (in.width + 1) / 2;
    const int chromaH = (in.height + 1) / 2;
    out.stride[0] = in.stride[0] > 0 ? in.stride[0] : in.width;
    out.stride[1] = chromaW;
    out.stride[2] = chromaW;
    out.plane[0] = in.plane[0];
    out.plane[1] = QByteArray(planeBytes(chromaW, chromaH), '\0');
    out.plane[2] = QByteArray(planeBytes(chromaW, chromaH), '\0');

    char* uu = out.plane[1].data();
    char* vv = out.plane[2].data();
    const char* src = in.plane[1].constData();
    for (int row = 0; row < chromaH; ++row) {
        const char* s = src + byteOffset(row, in.stride[1]);
        for (int x = 0; x < chromaW; ++x) {
            const qsizetype dstIndex = byteOffset(row, chromaW) + x;
            const qsizetype srcIndex = static_cast<qsizetype>(2) * x;
            uu[dstIndex] = s[srcIndex];
            vv[dstIndex] = s[srcIndex + 1];
        }
    }
    return out;
}

} // namespace

CpuFrameData::CpuFrameData(CpuPlanes planes) : m_planes(std::move(planes)) {}

CpuPlanes CpuFrameData::readToCpu(FramePixelFormat target) const {
    if (target == m_planes.format) return m_planes;
    if (m_planes.format == FramePixelFormat::Yuv420p && target == FramePixelFormat::Nv12)
        return convertYuv420pToNv12(m_planes);
    if (m_planes.format == FramePixelFormat::Nv12 && target == FramePixelFormat::Yuv420p)
        return convertNv12ToYuv420p(m_planes);
    return m_planes;
}

CpuPlanes FrameHandle::readToCpu(FramePixelFormat target) const {
    return m_data ? m_data->readToCpu(target) : CpuPlanes{};
}

bool FrameHandle::isValid() const {
    return m_data && m_meta.key.width > 0 && m_meta.key.height > 0 &&
           readToCpu(FramePixelFormat::Yuv420p).isValid();
}

bool FrameHandle::isPresentable() const {
    if (!m_data || m_meta.key.width <= 0 || m_meta.key.height <= 0) return false;
    if (m_data->isGpuBacked())
        return m_data->gpuSurface() != nullptr && planeCount(m_meta.key.format) > 0;
    return isValid();
}

bool FrameHandle::isStaleForGeneration(uint64_t currentGeneration) const {
    return m_meta.gpuGeneration != 0 && m_meta.gpuGeneration != currentGeneration;
}

MediaVideoFrameView::MediaVideoFrameView(const FrameHandle& handle) {
    const FrameMetadata& meta = handle.metadata();
    feedIndex = meta.key.feedIndex;
    ptsMs = meta.key.ptsMs;
    outputFrameIndex = meta.outputFrameIndex;
    width = meta.key.width;
    height = meta.key.height;
    isPlaceholder = meta.key.isPlaceholder;
    if (handle.isNull()) return;

    const CpuPlanes planes = handle.readToCpu(FramePixelFormat::Yuv420p);
    planeY = planes.plane[0];
    planeU = planes.plane[1];
    planeV = planes.plane[2];
    strideY = planes.stride[0];
    strideU = planes.stride[1];
    strideV = planes.stride[2];
}

FrameHandle makeCpuFrameHandle(CpuPlanes planes, FrameMetadata meta) {
    if (meta.key.width <= 0) meta.key.width = planes.width;
    if (meta.key.height <= 0) meta.key.height = planes.height;
    meta.key.format = planes.format;
    for (int i = 0; i < 3; ++i) {
        if (meta.stride[i] == 0) meta.stride[i] = planes.stride[i];
    }
    return FrameHandle(std::make_shared<CpuFrameData>(std::move(planes)), meta);
}

FrameHandle solidYuv420pHandle(int width, int height, uchar y, uchar u, uchar v) {
    const int chromaW = (width + 1) / 2;
    const int chromaH = (height + 1) / 2;

    CpuPlanes planes;
    planes.format = FramePixelFormat::Yuv420p;
    planes.width = width;
    planes.height = height;
    planes.stride[0] = width;
    planes.stride[1] = chromaW;
    planes.stride[2] = chromaW;
    planes.plane[0] = QByteArray(planeBytes(width, height), char(y));
    planes.plane[1] = QByteArray(planeBytes(chromaW, chromaH), char(u));
    planes.plane[2] = QByteArray(planeBytes(chromaW, chromaH), char(v));

    FrameMetadata meta;
    meta.key.format = FramePixelFormat::Yuv420p;
    meta.key.width = width;
    meta.key.height = height;
    meta.stride[0] = planes.stride[0];
    meta.stride[1] = planes.stride[1];
    meta.stride[2] = planes.stride[2];
    return makeCpuFrameHandle(std::move(planes), meta);
}
