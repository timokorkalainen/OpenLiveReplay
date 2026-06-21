#ifndef FRAMEHANDLE_H
#define FRAMEHANDLE_H

#include "playback/output/colormetadata.h"
#include "playback/output/framepixelformat.h"

#include <QByteArray>
#include <QtGlobal>

#include <memory>
#include <utility>

struct FramePayloadKey {
    int feedIndex = -1;
    qint64 ptsMs = 0;
    quint32 videoHash = 0;
    FramePixelFormat format = FramePixelFormat::Yuv420p;
    int width = 0;
    int height = 0;
    bool isPlaceholder = false;

    bool samePayloadAs(const FramePayloadKey& other) const {
        return feedIndex == other.feedIndex && ptsMs == other.ptsMs &&
               videoHash == other.videoHash && format == other.format && width == other.width &&
               height == other.height && isPlaceholder == other.isPlaceholder;
    }
};

struct FrameMetadata {
    FramePayloadKey key;
    qint64 outputFrameIndex = -1;
    qint64 sampledPlayheadMs = 0;
    int stride[3] = {0, 0, 0};
    ColorMetadata color;
};

struct CpuPlanes {
    FramePixelFormat format = FramePixelFormat::Yuv420p;
    int width = 0;
    int height = 0;
    QByteArray plane[3];
    int stride[3] = {0, 0, 0};

    bool isValid() const {
        if (width <= 0 || height <= 0) return false;
        if (format == FramePixelFormat::Yuv420p)
            return !plane[0].isEmpty() && !plane[1].isEmpty() && !plane[2].isEmpty();
        if (format == FramePixelFormat::Nv12) return !plane[0].isEmpty() && !plane[1].isEmpty();
        return !plane[0].isEmpty();
    }
};

class GpuSurface;

class IFrameData {
public:
    virtual ~IFrameData();

    virtual bool isGpuBacked() const = 0;
    bool isCpuBacked() const { return !isGpuBacked(); }
    virtual CpuPlanes readToCpu(FramePixelFormat target) const = 0;
    virtual GpuSurface* gpuSurface() const = 0;
    virtual FramePixelFormat nativeFormat() const = 0;
};

class CpuFrameData final : public IFrameData {
public:
    explicit CpuFrameData(CpuPlanes planes);

    bool isGpuBacked() const override { return false; }
    CpuPlanes readToCpu(FramePixelFormat target) const override;
    GpuSurface* gpuSurface() const override { return nullptr; }
    FramePixelFormat nativeFormat() const override { return m_planes.format; }

private:
    CpuPlanes m_planes;
};

class FrameHandle {
public:
    FrameHandle() = default;
    FrameHandle(std::shared_ptr<const IFrameData> data, FrameMetadata meta)
        : m_data(std::move(data)), m_meta(meta) {}

    bool isNull() const { return m_data == nullptr; }
    const FrameMetadata& metadata() const { return m_meta; }
    FrameMetadata& metadata() { return m_meta; }
    const IFrameData* data() const { return m_data.get(); }
    std::shared_ptr<const IFrameData> dataPtr() const { return m_data; }

    CpuPlanes readToCpu(FramePixelFormat target = FramePixelFormat::Yuv420p) const;
    bool isGpuBacked() const { return m_data && m_data->isGpuBacked(); }
    bool isValid() const;
    bool isPresentable() const { return isValid(); }

private:
    std::shared_ptr<const IFrameData> m_data;
    FrameMetadata m_meta;
};

struct MediaVideoFrameView {
    explicit MediaVideoFrameView(const FrameHandle& handle);

    int feedIndex = -1;
    qint64 ptsMs = 0;
    qint64 outputFrameIndex = -1;
    int width = 0;
    int height = 0;
    bool isPlaceholder = false;
    QByteArray planeY;
    QByteArray planeU;
    QByteArray planeV;
    int strideY = 0;
    int strideU = 0;
    int strideV = 0;

    bool isValid() const {
        return width > 0 && height > 0 && !planeY.isEmpty() && !planeU.isEmpty() &&
               !planeV.isEmpty();
    }
};

FrameHandle makeCpuFrameHandle(CpuPlanes planes, FrameMetadata meta);
FrameHandle solidYuv420pHandle(int width, int height, uchar y, uchar u, uchar v);

#endif // FRAMEHANDLE_H
