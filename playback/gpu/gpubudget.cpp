#include "playback/gpu/gpubudget.h"

#include "playback/gpu/gpusurface.h"

#include <QByteArray>
#include <QMutex>
#include <QMutexLocker>

#include <array>
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
qint64 g_gatedLiveBytes = 0;
qint64 g_oomDegrades = 0;
qint64 g_mintedBytesSinceLastSample = 0;
std::array<qint64, kGpuBudgetTagCount> g_liveBytesByTag{};

int tagIndex(GpuBudgetTag tag) {
    const int index = static_cast<int>(tag);
    return index >= 0 && index < kGpuBudgetTagCount ? index : static_cast<int>(GpuBudgetTag::Other);
}

bool reportOnlyEnabled() {
    const QByteArray value = qgetenv("OLR_LEDGER_REPORT_ONLY").toLower();
    return value == "1" || value == "true" || value == "yes" || value == "on";
}
} // namespace

const char* gpuBudgetTagName(GpuBudgetTag tag) {
    switch (tag) {
    case GpuBudgetTag::DecodeWindow:
        return "DecodeWindow";
    case GpuBudgetTag::Staging:
        return "Staging";
    case GpuBudgetTag::ReadbackRing:
        return "ReadbackRing";
    case GpuBudgetTag::CpuReadbackCache:
        return "CpuReadbackCache";
    case GpuBudgetTag::RetireQueue:
        return "RetireQueue";
    case GpuBudgetTag::RecorderWrap:
        return "RecorderWrap";
    case GpuBudgetTag::IngestWrap:
        return "IngestWrap";
    case GpuBudgetTag::OutputBus:
        return "OutputBus";
    case GpuBudgetTag::Other:
        return "Other";
    case GpuBudgetTag::Count:
        break;
    }
    return "Other";
}

bool gpuBudgetTagIsGated(GpuBudgetTag tag) {
    switch (tag) {
    case GpuBudgetTag::DecodeWindow:
    case GpuBudgetTag::Staging:
    case GpuBudgetTag::ReadbackRing:
    case GpuBudgetTag::OutputBus:
        return true;
    case GpuBudgetTag::CpuReadbackCache:
    case GpuBudgetTag::RetireQueue:
    case GpuBudgetTag::RecorderWrap:
    case GpuBudgetTag::IngestWrap:
    case GpuBudgetTag::Other:
    case GpuBudgetTag::Count:
        return false;
    }
    return false;
}

GpuBudget& GpuBudget::instance() {
    static GpuBudget budget;
    return budget;
}

void GpuBudget::configure(const GpuBudgetConfig& config) {
    QMutexLocker locker(&g_mutex);
    g_configured = true;
    g_budgetBytes = config.peakBudgetBytes();
}

void GpuBudget::setBudgetBytesForRuntime(qint64 bytes) {
    QMutexLocker locker(&g_mutex);
    if (bytes <= 0) return;
    g_configured = true;
    g_budgetBytes = bytes;
}

qint64 GpuBudget::budgetBytes() const {
    QMutexLocker locker(&g_mutex);
    return g_budgetBytes;
}

qint64 GpuBudget::liveBytes() const {
    QMutexLocker locker(&g_mutex);
    return g_liveBytes;
}

qint64 GpuBudget::liveBytes(GpuBudgetTag tag) const {
    QMutexLocker locker(&g_mutex);
    return g_liveBytesByTag[tagIndex(tag)];
}

qint64 GpuBudget::gatedLiveBytes() const {
    QMutexLocker locker(&g_mutex);
    return g_gatedLiveBytes;
}

qint64 GpuBudget::mintedBytesSinceLastSample() const {
    QMutexLocker locker(&g_mutex);
    return g_mintedBytesSinceLastSample;
}

void GpuBudget::resetMintedBytesSinceLastSample() {
    QMutexLocker locker(&g_mutex);
    g_mintedBytesSinceLastSample = 0;
}

bool GpuBudget::canAllocate(qint64 bytes) const {
    if (bytes <= 0) return true;
    QMutexLocker locker(&g_mutex);
    if (!g_configured) return true;
    return g_gatedLiveBytes + bytes <= g_budgetBytes;
}

std::optional<GpuBudgetCharge> GpuBudget::tryCharge(qint64 bytes, GpuBudgetTag tag) {
    if (bytes <= 0) return GpuBudgetCharge(0, tag, GpuBudgetCharge::Adopted{});

    QMutexLocker locker(&g_mutex);
    const bool gated = gpuBudgetTagIsGated(tag);
    if (gated && g_configured && g_gatedLiveBytes + bytes > g_budgetBytes) return std::nullopt;
    g_liveBytes += bytes;
    g_liveBytesByTag[tagIndex(tag)] += bytes;
    g_mintedBytesSinceLastSample += bytes;
    if (gated) g_gatedLiveBytes += bytes;
    return GpuBudgetCharge(bytes, tag, GpuBudgetCharge::Adopted{});
}

void GpuBudget::charge(qint64 bytes, GpuBudgetTag tag) {
    if (bytes <= 0) return;
    QMutexLocker locker(&g_mutex);
    g_liveBytes += bytes;
    g_liveBytesByTag[tagIndex(tag)] += bytes;
    g_mintedBytesSinceLastSample += bytes;
    if (gpuBudgetTagIsGated(tag)) g_gatedLiveBytes += bytes;
}

void GpuBudget::credit(qint64 bytes, GpuBudgetTag tag) {
    if (bytes <= 0) return;
    QMutexLocker locker(&g_mutex);
    g_liveBytes -= bytes;
    if (g_liveBytes < 0) g_liveBytes = 0;
    qint64& taggedBytes = g_liveBytesByTag[tagIndex(tag)];
    taggedBytes -= bytes;
    if (taggedBytes < 0) taggedBytes = 0;
    if (gpuBudgetTagIsGated(tag)) {
        g_gatedLiveBytes -= bytes;
        if (g_gatedLiveBytes < 0) g_gatedLiveBytes = 0;
    }
}

qint64 GpuBudget::oomDegradeCount() const {
    QMutexLocker locker(&g_mutex);
    return g_oomDegrades;
}

void GpuBudget::noteOomDegrade() {
    QMutexLocker locker(&g_mutex);
    ++g_oomDegrades;
}

GpuBudgetSnapshot GpuBudget::snapshot() const {
    QMutexLocker locker(&g_mutex);
    GpuBudgetSnapshot snapshot;
    snapshot.budgetBytes = g_budgetBytes;
    snapshot.liveBytes = g_liveBytes;
    snapshot.gatedLiveBytes = g_gatedLiveBytes;
    snapshot.oomDegrades = g_oomDegrades;
    snapshot.reportOnly = reportOnlyEnabled();
    snapshot.liveBytesByTag = g_liveBytesByTag;
    return snapshot;
}

void GpuBudget::reset() {
    QMutexLocker locker(&g_mutex);
    g_liveBytes = 0;
    g_gatedLiveBytes = 0;
    g_oomDegrades = 0;
    g_mintedBytesSinceLastSample = 0;
    g_liveBytesByTag.fill(0);
}

GpuBudgetCharge::GpuBudgetCharge(qint64 bytes)
    : GpuBudgetCharge(bytes, GpuBudgetTag::DecodeWindow) {}

GpuBudgetCharge::GpuBudgetCharge(qint64 bytes, GpuBudgetTag tag)
    : m_bytes(bytes > 0 ? bytes : 0), m_tag(tag) {
    if (m_bytes > 0) GpuBudget::instance().charge(m_bytes, m_tag);
}

GpuBudgetCharge::GpuBudgetCharge(qint64 bytes, GpuBudgetTag tag, Adopted)
    : m_bytes(bytes > 0 ? bytes : 0), m_tag(tag) {}

GpuBudgetCharge::~GpuBudgetCharge() {
    if (m_bytes > 0) GpuBudget::instance().credit(m_bytes, m_tag);
}

GpuBudgetCharge::GpuBudgetCharge(GpuBudgetCharge&& other) noexcept
    : m_bytes(other.m_bytes), m_tag(other.m_tag) {
    other.m_bytes = 0;
}

GpuBudgetCharge& GpuBudgetCharge::operator=(GpuBudgetCharge&& other) noexcept {
    if (this != &other) {
        if (m_bytes > 0) GpuBudget::instance().credit(m_bytes, m_tag);
        m_bytes = other.m_bytes;
        m_tag = other.m_tag;
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
