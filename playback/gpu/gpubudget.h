#ifndef OLR_GPUBUDGET_H
#define OLR_GPUBUDGET_H

#include "playback/output/framepixelformat.h"

#include <QtGlobal>

#include <array>
#include <optional>

class GpuSurface;

enum class GpuBudgetTag {
    DecodeWindow = 0,
    Staging,
    ReadbackRing,
    CpuReadbackCache,
    RetireQueue,
    RecorderWrap,
    IngestWrap,
    OutputBus,
    Other,
    Count
};

constexpr int kGpuBudgetTagCount = static_cast<int>(GpuBudgetTag::Count);

const char* gpuBudgetTagName(GpuBudgetTag tag);
bool gpuBudgetTagIsGated(GpuBudgetTag tag);

struct GpuBudgetSnapshot {
    qint64 budgetBytes = 0;
    qint64 liveBytes = 0;
    qint64 gatedLiveBytes = 0;
    qint64 oomDegrades = 0;
    bool reportOnly = false;
    std::array<qint64, kGpuBudgetTagCount> liveBytesByTag{};
};

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
    GpuBudgetCharge(qint64 bytes, GpuBudgetTag tag);
    ~GpuBudgetCharge();

    GpuBudgetCharge(GpuBudgetCharge&& other) noexcept;
    GpuBudgetCharge& operator=(GpuBudgetCharge&& other) noexcept;

    GpuBudgetCharge(const GpuBudgetCharge&) = delete;
    GpuBudgetCharge& operator=(const GpuBudgetCharge&) = delete;

    qint64 bytes() const { return m_bytes; }
    GpuBudgetTag tag() const { return m_tag; }

private:
    struct Adopted {};
    GpuBudgetCharge(qint64 bytes, GpuBudgetTag tag, Adopted);

    qint64 m_bytes = 0;
    GpuBudgetTag m_tag = GpuBudgetTag::DecodeWindow;

    friend class GpuBudget;
};

class GpuBudget {
public:
    static GpuBudget& instance();

    void configure(const GpuBudgetConfig& config);
    void setBudgetBytesForRuntime(qint64 bytes);
    qint64 budgetBytes() const;
    qint64 liveBytes() const;
    qint64 liveBytes(GpuBudgetTag tag) const;
    qint64 gatedLiveBytes() const;
    qint64 mintedBytesSinceLastSample() const;
    void resetMintedBytesSinceLastSample();
    bool canAllocate(qint64 bytes) const;
    std::optional<GpuBudgetCharge> tryCharge(qint64 bytes,
                                             GpuBudgetTag tag = GpuBudgetTag::DecodeWindow);
    void charge(qint64 bytes, GpuBudgetTag tag = GpuBudgetTag::DecodeWindow);
    void credit(qint64 bytes, GpuBudgetTag tag = GpuBudgetTag::DecodeWindow);
    qint64 oomDegradeCount() const;
    void noteOomDegrade();
    GpuBudgetSnapshot snapshot() const;
    void reset();

private:
    GpuBudget() = default;
};

qint64 gpuSurfaceBytes(const GpuSurface& surface);

#endif // OLR_GPUBUDGET_H
