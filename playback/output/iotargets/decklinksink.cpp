#include "playback/output/iotargets/decklinksink.h"

#include "playback/gpu/gpufence.h"
#include "playback/gpu/gpusurface.h"
#include "playback/output/iotargets/sinkcapabilityprobe.h"

#include <QElapsedTimer>
#include <QMutexLocker>

#include <utility>

namespace {
constexpr int kGpuSubmitFenceTimeoutMs = 2000;
constexpr quint32 kDeckLinkSt2110Ssrc = 0x4f4c5231u; // "OLR1"

bool hasValidNativeGpuSurface(const FrameHandle& frame) {
    if (!frame.isGpuBacked() || !frame.isPresentable()) {
        return false;
    }
    const IFrameData* data = frame.data();
    GpuSurface* surface = data ? data->gpuSurface() : nullptr;
    return surface && surface->isValid() && surface->hasNativeBacking();
}

bool waitForNativeGpuProducer(const FrameHandle& frame) {
    const IFrameData* data = frame.data();
    GpuSurface* surface = data ? data->gpuSurface() : nullptr;
    if (!surface) return false;

    const std::shared_ptr<GpuFence> fence = data->gpuFence();
    if (!fence) return false;

    const uint64_t pendingFenceValue = surface->pendingFenceValue();
    if (pendingFenceValue == 0) return false;
    return fence->wait(pendingFenceValue, kGpuSubmitFenceTimeoutMs);
}

QByteArray st2110EssenceForFrame(const OutputBusFrame& frame) {
    QByteArray essence;
    if (!frame.video.isGpuBacked()) {
        const CpuPlanes planes = frame.video.readToCpu(FramePixelFormat::Yuv420p);
        if (planes.isValid()) {
            essence.reserve(planes.plane[0].size() + planes.plane[1].size() +
                            planes.plane[2].size());
            essence.append(planes.plane[0]);
            essence.append(planes.plane[1]);
            essence.append(planes.plane[2]);
        }
    }
    if (essence.isEmpty()) {
        essence = QByteArray::number(frame.outputFrameIndex) + ':' +
                  QByteArray::number(frame.identity.videoHash) + ':' +
                  QByteArray::number(frame.video.metadata().gpuGeneration);
    }
    return essence;
}

St2110VideoFrame st2110FrameFor(const OutputBusFrame& frame, FrameRate rate) {
    St2110FrameFramer framer(rate, kDeckLinkSt2110Ssrc);
    return framer.frameVideo(st2110EssenceForFrame(frame), frame.outputFrameIndex,
                             frame.programmeTimecode100ns);
}
} // namespace

#ifndef OLR_WITH_DECKLINK_BUILD
std::unique_ptr<IDeckLinkSenderBackend> makeDeckLinkSenderBackend() {
    return std::make_unique<StubDeckLinkSenderBackend>();
}
#endif

DeckLinkOutputSink::DeckLinkOutputSink(OutputTargetKind kind)
    : m_kind(kind), m_ownedBackend(makeDeckLinkSenderBackend()) {
    m_backend = m_ownedBackend.get();
}

DeckLinkOutputSink::DeckLinkOutputSink(OutputTargetKind kind, IDeckLinkSenderBackend* backend)
    : m_kind(kind), m_backend(backend) {}

DeckLinkOutputSink::~DeckLinkOutputSink() {
    stop();
}

bool DeckLinkOutputSink::start(const OutputTargetAssignment& assignment, FrameRate rate) {
    stop();
    resetCounters();

    if (!m_backend || !isDeckLinkKind(m_kind) || assignment.kind != m_kind || !assignment.enabled ||
        !rate.isValid()) {
        setStatus(DeckLinkOutputState::InvalidAssignment,
                  QStringLiteral("invalid DeckLink output assignment"));
        return false;
    }

    if (!m_backend->isRuntimeAvailable()) {
        setStatus(DeckLinkOutputState::RuntimeUnavailable,
                  QStringLiteral("DeckLink runtime/SDK is not available"));
        return false;
    }

    if (!m_backend->openDevice(assignment, rate)) {
        setStatus(DeckLinkOutputState::OpenFailed,
                  QStringLiteral("failed to open DeckLink device"));
        return false;
    }

    m_capability = capabilityForAssignment(assignment);
    m_assignment = assignment;
    m_rate = rate;
    m_active = true;
    setStatus(DeckLinkOutputState::Active, QStringLiteral("DeckLink output active"));
    return true;
}

void DeckLinkOutputSink::stop() {
    if (m_active && m_backend) m_backend->closeDevice();
    m_active = false;
    setStatus(DeckLinkOutputState::Stopped, QStringLiteral("DeckLink output stopped"));
}

bool DeckLinkOutputSink::submit(const OutputBusFrame& frame) {
    if (!m_active || !m_backend) return false;

    QElapsedTimer timer;
    timer.start();
    {
        QMutexLocker locker(&m_statusMutex);
        m_lastFrameIdentity = outputFrameIdentityFor(frame);
        m_hasLastFrameIdentity = true;
        m_lastFrameDelivered = false;
    }

    const bool useGpuSubmission =
        m_capability == SinkGpuCapability::GpuNative && hasValidNativeGpuSurface(frame.video);
    if (m_capability == SinkGpuCapability::GpuNative && !useGpuSubmission) {
        QMutexLocker locker(&m_statusMutex);
        ++m_sendFailures;
        m_lastSubmitDurationNs = timer.nsecsElapsed();
        m_lastFrameDelivered = false;
        m_state = DeckLinkOutputState::SendFailed;
        m_message = QStringLiteral("DeckLink GPU-native frame lacks a valid native GPU surface");
        return false;
    }
    if (useGpuSubmission && !waitForNativeGpuProducer(frame.video)) {
        QMutexLocker locker(&m_statusMutex);
        ++m_sendFailures;
        m_lastSubmitDurationNs = timer.nsecsElapsed();
        m_lastFrameDelivered = false;
        m_state = DeckLinkOutputState::SendFailed;
        m_message = QStringLiteral("DeckLink GPU frame producer fence did not retire");
        return false;
    }
    OutputBusFrame scheduledFrame = frame;
    const bool isSt2110 = m_kind == OutputTargetKind::DeckLinkIpSt2110;
    const bool ok =
        isSt2110
            ? (useGpuSubmission ? m_backend->scheduleGpuSt2110Frame(std::move(scheduledFrame),
                                                                    st2110FrameFor(frame, m_rate))
                                : m_backend->scheduleSt2110Frame(std::move(scheduledFrame),
                                                                 st2110FrameFor(frame, m_rate)))
            : (useGpuSubmission ? m_backend->scheduleGpuFrame(std::move(scheduledFrame))
                                : m_backend->scheduleFrame(std::move(scheduledFrame)));
    {
        QMutexLocker locker(&m_statusMutex);
        m_lastSubmitDurationNs = timer.nsecsElapsed();
        if (ok) {
            ++m_framesSubmitted;
            m_lastFrameDelivered = true;
            m_state = DeckLinkOutputState::Active;
            m_message = QStringLiteral("DeckLink output active");
        } else {
            ++m_sendFailures;
            m_lastFrameDelivered = false;
            m_state = DeckLinkOutputState::SendFailed;
            m_message = QStringLiteral("failed to schedule DeckLink frame");
        }
    }
    return ok;
}

OutputSinkStatus DeckLinkOutputSink::outputStatus() const {
    QMutexLocker locker(&m_statusMutex);
    OutputSinkStatus out;
    out.acceptedFrames = m_framesSubmitted;
    out.failedFrames = m_sendFailures;
    out.lastSubmitDurationNs = m_lastSubmitDurationNs;
    out.hasLastResult = m_framesSubmitted > 0 || m_sendFailures > 0;
    out.lastResultSucceeded = m_state != DeckLinkOutputState::SendFailed;

    if (m_hasLastFrameIdentity) {
        out.hasLastQueuedFrameIndex = true;
        out.lastQueuedFrameIndex = m_lastFrameIdentity.outputFrameIndex;
        out.hasLastDeliveredFrameIndex =
            m_lastFrameDelivered && m_state == DeckLinkOutputState::Active;
        if (out.hasLastDeliveredFrameIndex) {
            out.lastDeliveredFrameIndex = m_lastFrameIdentity.outputFrameIndex;
        }
    }

    switch (m_state) {
    case DeckLinkOutputState::Stopped:
        out.state = QStringLiteral("stopped");
        break;
    case DeckLinkOutputState::RuntimeUnavailable:
        out.state = QStringLiteral("runtime-unavailable");
        break;
    case DeckLinkOutputState::InvalidAssignment:
        out.state = QStringLiteral("invalid");
        break;
    case DeckLinkOutputState::OpenFailed:
        out.state = QStringLiteral("open-failed");
        break;
    case DeckLinkOutputState::Active:
        out.state = QStringLiteral("active");
        break;
    case DeckLinkOutputState::SendFailed:
        out.state = QStringLiteral("send-failed");
        break;
    }
    out.message = m_message;
    return out;
}

SinkGpuCapability DeckLinkOutputSink::resolvedCapability() const {
    return m_capability;
}

SinkGpuCapability
DeckLinkOutputSink::capabilityForAssignment(const OutputTargetAssignment& assignment) const {
    const bool runtimeAvailable = m_backend && m_backend->isRuntimeAvailable();
    const bool gpuTextureInput = runtimeAvailable && m_backend->deviceSupportsGpuTextureInput();
    return SinkCapabilityProbe::classify(m_kind, assignment.settings, runtimeAvailable,
                                         gpuTextureInput);
}

bool DeckLinkOutputSink::isDeckLinkKind(OutputTargetKind kind) {
    return kind == OutputTargetKind::DeckLinkSdiHdmi || kind == OutputTargetKind::DeckLinkIpSt2110;
}

void DeckLinkOutputSink::resetCounters() {
    QMutexLocker locker(&m_statusMutex);
    m_framesSubmitted = 0;
    m_sendFailures = 0;
    m_lastSubmitDurationNs = 0;
    m_hasLastFrameIdentity = false;
    m_lastFrameDelivered = false;
    m_lastFrameIdentity = OutputFrameIdentity();
}

void DeckLinkOutputSink::setStatus(DeckLinkOutputState state, const QString& message) {
    QMutexLocker locker(&m_statusMutex);
    m_state = state;
    m_message = message;
}
