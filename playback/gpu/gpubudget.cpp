#include "playback/gpu/gpubudget.h"

#include "playback/gpu/gpusurface.h"

#include <QMutex>
#include <QMutexLocker>

#include <optional>
#include <utility>

namespace {

qint64 bytesForSurface(int width, int height, FramePixelFormat format) {
    const qint64 w = qMax(0, width);
    const qint64 h = qMax(0, height);
    switch (format) {
    case FramePixelFormat::Nv12:
    case FramePixelFormat::Yuv420p:
        return w * h * 3 / 2;
    case FramePixelFormat::Rgba8:
        return w * h * 4;
    }
    return w * h * 3 / 2;
}

} // namespace

qint64 GpuBudgetConfig::surfaceBytes() const {
    return bytesForSurface(surfaceWidth > 0 ? surfaceWidth : width,
                           surfaceHeight > 0 ? surfaceHeight : height, surfaceFormat);
}

qint64 GpuBudgetConfig::outputSurfaceBytes() const {
    return bytesForSurface(outputWidth > 0 ? outputWidth : width,
                           outputHeight > 0 ? outputHeight : height, outputSurfaceFormat);
}

qint64 GpuBudgetConfig::readbackSurfaceBytes() const {
    return bytesForSurface(readbackWidth > 0 ? readbackWidth : width,
                           readbackHeight > 0 ? readbackHeight : height, readbackSurfaceFormat);
}

qint64 GpuBudgetConfig::peakBudgetBytes() const {
    const qint64 decode = qMax(0, aggregateDecodeWindow);
    const qint64 staging = qint64(qMax(0, stagingWindowPerFeed)) * qMax(1, feedCount);
    const qint64 busOutputs = qMax(0, activeBusCount);
    const qint64 readbackRings = qint64(qMax(0, readbackRingDepth)) * qMax(0, activeBusCount);
    return (decode + staging) * surfaceBytes() + busOutputs * outputSurfaceBytes() +
           readbackRings * readbackSurfaceBytes();
}
namespace {
QMutex g_mutex;
bool g_configured = false;
qint64 g_budgetBytes = 0;
qint64 g_liveBytes = 0;
qint64 g_oomDegrades = 0;
} // namespace

GpuBudget& GpuBudget::instance() {
    static GpuBudget budget;
    return budget;
}

void GpuBudget::configure(const GpuBudgetConfig& config) {
    QMutexLocker locker(&g_mutex);
    g_configured = true;
    g_budgetBytes = config.peakBudgetBytes();
}

qint64 GpuBudget::budgetBytes() const {
    QMutexLocker locker(&g_mutex);
    return g_budgetBytes;
}

qint64 GpuBudget::liveBytes() const {
    QMutexLocker locker(&g_mutex);
    return g_liveBytes;
}

bool GpuBudget::canAllocate(qint64 bytes) const {
    if (bytes <= 0) return true;
    QMutexLocker locker(&g_mutex);
    if (!g_configured) return true;
    return g_liveBytes + bytes <= g_budgetBytes;
}

std::optional<GpuBudgetCharge> GpuBudget::tryCharge(qint64 bytes) {
    if (bytes <= 0) return GpuBudgetCharge(0, GpuBudgetCharge::Adopted{});

    QMutexLocker locker(&g_mutex);
    if (g_configured && g_liveBytes + bytes > g_budgetBytes) return std::nullopt;
    g_liveBytes += bytes;
    return GpuBudgetCharge(bytes, GpuBudgetCharge::Adopted{});
}

void GpuBudget::charge(qint64 bytes) {
    if (bytes <= 0) return;
    QMutexLocker locker(&g_mutex);
    g_liveBytes += bytes;
}

void GpuBudget::credit(qint64 bytes) {
    if (bytes <= 0) return;
    QMutexLocker locker(&g_mutex);
    g_liveBytes -= bytes;
    if (g_liveBytes < 0) g_liveBytes = 0;
}

qint64 GpuBudget::oomDegradeCount() const {
    QMutexLocker locker(&g_mutex);
    return g_oomDegrades;
}

void GpuBudget::noteOomDegrade() {
    QMutexLocker locker(&g_mutex);
    ++g_oomDegrades;
}

void GpuBudget::reset() {
    QMutexLocker locker(&g_mutex);
    g_liveBytes = 0;
    g_oomDegrades = 0;
}

GpuBudgetCharge::GpuBudgetCharge(qint64 bytes) : m_bytes(bytes > 0 ? bytes : 0) {
    if (m_bytes > 0) GpuBudget::instance().charge(m_bytes);
}

GpuBudgetCharge::GpuBudgetCharge(qint64 bytes, Adopted) : m_bytes(bytes > 0 ? bytes : 0) {}

GpuBudgetCharge::~GpuBudgetCharge() {
    if (m_bytes > 0) GpuBudget::instance().credit(m_bytes);
}

GpuBudgetCharge::GpuBudgetCharge(GpuBudgetCharge&& other) noexcept : m_bytes(other.m_bytes) {
    other.m_bytes = 0;
}

GpuBudgetCharge& GpuBudgetCharge::operator=(GpuBudgetCharge&& other) noexcept {
    if (this != &other) {
        if (m_bytes > 0) GpuBudget::instance().credit(m_bytes);
        m_bytes = other.m_bytes;
        other.m_bytes = 0;
    }
    return *this;
}

qint64 gpuSurfaceBytes(const GpuSurface& surface) {
    const GpuSurfaceDesc desc = surface.desc();
    if (desc.allocationBytes > 0) return desc.allocationBytes;
    GpuBudgetConfig config;
    config.width = desc.width;
    config.height = desc.height;
    config.surfaceFormat = desc.format;
    return config.surfaceBytes();
}
