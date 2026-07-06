#ifndef OLR_GPUBUDGET_H
#define OLR_GPUBUDGET_H

#include "playback/output/framepixelformat.h"

#include <QtGlobal>

#include <optional>

class GpuSurface;

struct GpuBudgetConfig {
    int feedCount = 1;
    int aggregateDecodeWindow = 256;
    int stagingWindowPerFeed = 0;
    int activeBusCount = 1;
    int readbackRingDepth = 3;
    int width = 1920;
    int height = 1080;
    int surfaceWidth = 0;
    int surfaceHeight = 0;
    int outputWidth = 0;
    int outputHeight = 0;
    int readbackWidth = 0;
    int readbackHeight = 0;
    FramePixelFormat surfaceFormat = FramePixelFormat::Nv12;
    FramePixelFormat outputSurfaceFormat = FramePixelFormat::Rgba8;
    FramePixelFormat readbackSurfaceFormat = FramePixelFormat::Nv12;

    qint64 surfaceBytes() const;
    qint64 outputSurfaceBytes() const;
    qint64 readbackSurfaceBytes() const;
    qint64 peakBudgetBytes() const;
};

class GpuBudgetCharge {
public:
    GpuBudgetCharge() = default;
    explicit GpuBudgetCharge(qint64 bytes);
    ~GpuBudgetCharge();

    GpuBudgetCharge(GpuBudgetCharge&& other) noexcept;
    GpuBudgetCharge& operator=(GpuBudgetCharge&& other) noexcept;

    GpuBudgetCharge(const GpuBudgetCharge&) = delete;
    GpuBudgetCharge& operator=(const GpuBudgetCharge&) = delete;

    qint64 bytes() const { return m_bytes; }

private:
    struct Adopted {};
    GpuBudgetCharge(qint64 bytes, Adopted);

    qint64 m_bytes = 0;

    friend class GpuBudget;
};

class GpuBudget {
public:
    static GpuBudget& instance();

    void configure(const GpuBudgetConfig& config);
    qint64 budgetBytes() const;
    qint64 liveBytes() const;
    bool canAllocate(qint64 bytes) const;
    std::optional<GpuBudgetCharge> tryCharge(qint64 bytes);
    void charge(qint64 bytes);
    void credit(qint64 bytes);
    qint64 oomDegradeCount() const;
    void noteOomDegrade();
    void reset();

private:
    GpuBudget() = default;
};

qint64 gpuSurfaceBytes(const GpuSurface& surface);

#endif // OLR_GPUBUDGET_H
