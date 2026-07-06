#include "playback/output/iotargets/iotargetsinkfactory.h"

#include "playback/output/asyncgpureadbacksink.h"
#include "playback/output/framepixelformat.h"
#include "playback/output/iotargets/ajasink.h"
#include "playback/output/iotargets/decklinksink.h"
#include "playback/output/iotargets/omtsink.h"
#include "playback/output/sinkgpucapability.h"

#include <functional>
#include <QtGlobal>

namespace {

std::unique_ptr<IOutputSink> wrapReadback(std::unique_ptr<IOutputSink> sink,
                                          const OutputTargetAssignment& assignment,
                                          SinkGpuCapability capability,
                                          std::shared_ptr<GpuFence> renderFence,
                                          std::shared_ptr<SharedGpuReadbackCache> sharedReadbacks) {
    if (capability == SinkGpuCapability::GpuNative) return sink;

    const int ringDepth = assignment.sourceBus.kind == OutputBusKind::Pgm ? 1 : 3;
    return std::make_unique<AsyncGpuReadbackSink>(
        std::move(sink), ringDepth, FramePixelFormat::Yuv420p, capability, std::move(renderFence),
        std::move(sharedReadbacks));
}

class DeckLinkCapabilityRoutingSink final : public IOutputSink {
public:
    using SinkFactory = std::function<std::unique_ptr<DeckLinkOutputSink>()>;

    DeckLinkCapabilityRoutingSink(OutputTargetKind kind, SinkFactory factory,
                                  std::shared_ptr<GpuFence> renderFence,
                                  std::shared_ptr<SharedGpuReadbackCache> sharedReadbacks)
        : m_kind(kind), m_factory(std::move(factory)), m_renderFence(std::move(renderFence)),
          m_sharedReadbacks(std::move(sharedReadbacks)) {}

    ~DeckLinkCapabilityRoutingSink() override { stop(); }

    OutputTargetKind kind() const override { return m_kind; }

    bool start(const OutputTargetAssignment& assignment, FrameRate rate) override {
        stop();
        if (!m_factory) return false;

        m_direct = m_factory();
        if (!m_direct) return false;
        if (!m_direct->start(assignment, rate)) {
            m_active = m_direct.get();
            return false;
        }

        const SinkGpuCapability capability = m_direct->resolvedCapability();
        if (capability == SinkGpuCapability::GpuNative) {
            m_active = m_direct.get();
            return true;
        }

        const int ringDepth = assignment.sourceBus.kind == OutputBusKind::Pgm ? 1 : 3;
        m_readback = std::make_unique<AsyncGpuReadbackSink>(std::move(m_direct), ringDepth,
                                                            FramePixelFormat::Yuv420p, capability,
                                                            m_renderFence, m_sharedReadbacks, true);
        if (!m_readback->start(assignment, rate)) {
            m_readback.reset();
            return false;
        }
        m_active = m_readback.get();
        return true;
    }

    void stop() override {
        if (m_readback) {
            m_readback->stop();
        } else if (m_direct) {
            m_direct->stop();
        }
        m_active = nullptr;
        m_readback.reset();
        m_direct.reset();
    }

    bool isActive() const override { return m_active && m_active->isActive(); }

    bool submit(const OutputBusFrame& frame) override {
        return m_active && m_active->submit(frame);
    }

    OutputSinkStatus outputStatus() const override {
        if (m_active) return m_active->outputStatus();
        if (m_direct) return m_direct->outputStatus();
        return OutputSinkStatus{};
    }

    bool readbackStats(qint64& depth, qint64& drops) const override {
        return m_readback && m_readback->readbackStats(depth, drops);
    }

    bool needsContinuousCadence() const override { return true; }

private:
    OutputTargetKind m_kind;
    SinkFactory m_factory;
    std::shared_ptr<GpuFence> m_renderFence;
    std::shared_ptr<SharedGpuReadbackCache> m_sharedReadbacks;
    std::unique_ptr<DeckLinkOutputSink> m_direct;
    std::unique_ptr<AsyncGpuReadbackSink> m_readback;
    IOutputSink* m_active = nullptr;
};

std::unique_ptr<IOutputSink>
makeDeckLinkRoutingSink(OutputTargetKind kind, DeckLinkCapabilityRoutingSink::SinkFactory factory,
                        std::shared_ptr<GpuFence> renderFence,
                        std::shared_ptr<SharedGpuReadbackCache> sharedReadbacks) {
    return std::make_unique<DeckLinkCapabilityRoutingSink>(
        kind, std::move(factory), std::move(renderFence), std::move(sharedReadbacks));
}

} // namespace

std::unique_ptr<IOutputSink>
makeIoTargetSink(const OutputTargetAssignment& assignment, FrameRate rate,
                 std::shared_ptr<GpuFence> renderFence,
                 std::shared_ptr<SharedGpuReadbackCache> sharedReadbacks) {
    Q_UNUSED(rate);
    switch (assignment.kind) {
    case OutputTargetKind::DeckLinkSdiHdmi:
    case OutputTargetKind::DeckLinkIpSt2110:
        return makeDeckLinkRoutingSink(
            assignment.kind,
            [kind = assignment.kind] { return std::make_unique<DeckLinkOutputSink>(kind); },
            std::move(renderFence), std::move(sharedReadbacks));
    case OutputTargetKind::Aja: {
        auto sink = std::make_unique<AjaOutputSink>();
        const SinkGpuCapability capability = sink->resolvedCapability();
        return wrapReadback(std::move(sink), assignment, capability, std::move(renderFence),
                            std::move(sharedReadbacks));
    }
    case OutputTargetKind::Omt: {
        auto sink = std::make_unique<OmtOutputSink>();
        const SinkGpuCapability capability = sink->resolvedCapability();
        return wrapReadback(std::move(sink), assignment, capability, std::move(renderFence),
                            std::move(sharedReadbacks));
    }
    case OutputTargetKind::QtPreview:
    case OutputTargetKind::Ndi:
        return nullptr;
    }

    return nullptr;
}

#ifdef OLR_UNIT_TEST
std::unique_ptr<IOutputSink>
makeDeckLinkIoTargetSinkForTest(OutputTargetKind kind, IDeckLinkSenderBackend* backend,
                                std::shared_ptr<GpuFence> renderFence,
                                std::shared_ptr<SharedGpuReadbackCache> sharedReadbacks) {
    return makeDeckLinkRoutingSink(
        kind, [kind, backend] { return std::make_unique<DeckLinkOutputSink>(kind, backend); },
        std::move(renderFence), std::move(sharedReadbacks));
}
#endif
