#include "playback/output/iotargets/ajasink.h"

#include <QElapsedTimer>
#include <QMutexLocker>

#ifndef OLR_WITH_AJA_BUILD
std::unique_ptr<IAjaSenderBackend> makeAjaSenderBackend() {
    return std::make_unique<StubAjaSenderBackend>();
}
#endif

AjaOutputSink::AjaOutputSink() : m_ownedBackend(makeAjaSenderBackend()) {
    m_backend = m_ownedBackend.get();
}

AjaOutputSink::AjaOutputSink(IAjaSenderBackend* backend) : m_backend(backend) {}

AjaOutputSink::~AjaOutputSink() {
    stop();
}

bool AjaOutputSink::start(const OutputTargetAssignment& assignment, FrameRate rate) {
    stop();
    resetCounters();

    if (!m_backend || assignment.kind != OutputTargetKind::Aja || !assignment.enabled ||
        !rate.isValid()) {
        setStatus(AjaOutputState::InvalidAssignment,
                  QStringLiteral("invalid AJA output assignment"));
        return false;
    }

    if (!m_backend->isRuntimeAvailable()) {
        setStatus(AjaOutputState::RuntimeUnavailable,
                  QStringLiteral("AJA NTV2 runtime/SDK is not available"));
        return false;
    }

    if (!m_backend->openDevice(assignment, rate)) {
        setStatus(AjaOutputState::OpenFailed, QStringLiteral("failed to open AJA device"));
        return false;
    }

    m_assignment = assignment;
    m_rate = rate;
    m_active = true;
    setStatus(AjaOutputState::Active, QStringLiteral("AJA output active"));
    return true;
}

void AjaOutputSink::stop() {
    if (m_active && m_backend) m_backend->closeDevice();
    m_active = false;
    setStatus(AjaOutputState::Stopped, QStringLiteral("AJA output stopped"));
}

bool AjaOutputSink::submit(const OutputBusFrame& frame) {
    if (!m_active || !m_backend) return false;

    QElapsedTimer timer;
    timer.start();
    {
        QMutexLocker locker(&m_statusMutex);
        m_lastFrameIdentity = outputFrameIdentityFor(frame);
        m_hasLastFrameIdentity = true;
        m_lastFrameDelivered = false;
    }

    const bool ok = m_backend->transferFrame(frame);
    {
        QMutexLocker locker(&m_statusMutex);
        m_lastSubmitDurationNs = timer.nsecsElapsed();
        if (ok) {
            ++m_framesSubmitted;
            m_lastFrameDelivered = true;
            m_state = AjaOutputState::Active;
            m_message = QStringLiteral("AJA output active");
        } else {
            ++m_sendFailures;
            m_lastFrameDelivered = false;
            m_state = AjaOutputState::SendFailed;
            m_message = QStringLiteral("failed to transfer AJA frame");
        }
    }
    return ok;
}

OutputSinkStatus AjaOutputSink::outputStatus() const {
    QMutexLocker locker(&m_statusMutex);
    OutputSinkStatus out;
    out.acceptedFrames = m_framesSubmitted;
    out.failedFrames = m_sendFailures;
    out.lastSubmitDurationNs = m_lastSubmitDurationNs;
    out.hasLastResult = m_framesSubmitted > 0 || m_sendFailures > 0;
    out.lastResultSucceeded = m_state != AjaOutputState::SendFailed;

    if (m_hasLastFrameIdentity) {
        out.hasLastQueuedFrameIndex = true;
        out.lastQueuedFrameIndex = m_lastFrameIdentity.outputFrameIndex;
        out.hasLastDeliveredFrameIndex = m_lastFrameDelivered && m_state == AjaOutputState::Active;
        if (out.hasLastDeliveredFrameIndex) {
            out.lastDeliveredFrameIndex = m_lastFrameIdentity.outputFrameIndex;
        }
    }

    switch (m_state) {
    case AjaOutputState::Stopped:
        out.state = QStringLiteral("stopped");
        break;
    case AjaOutputState::RuntimeUnavailable:
        out.state = QStringLiteral("runtime-unavailable");
        break;
    case AjaOutputState::InvalidAssignment:
        out.state = QStringLiteral("invalid");
        break;
    case AjaOutputState::OpenFailed:
        out.state = QStringLiteral("open-failed");
        break;
    case AjaOutputState::Active:
        out.state = QStringLiteral("active");
        break;
    case AjaOutputState::SendFailed:
        out.state = QStringLiteral("send-failed");
        break;
    }
    out.message = m_message;
    return out;
}

void AjaOutputSink::resetCounters() {
    QMutexLocker locker(&m_statusMutex);
    m_framesSubmitted = 0;
    m_sendFailures = 0;
    m_lastSubmitDurationNs = 0;
    m_hasLastFrameIdentity = false;
    m_lastFrameDelivered = false;
    m_lastFrameIdentity = OutputFrameIdentity();
}

void AjaOutputSink::setStatus(AjaOutputState state, const QString& message) {
    QMutexLocker locker(&m_statusMutex);
    m_state = state;
    m_message = message;
}
