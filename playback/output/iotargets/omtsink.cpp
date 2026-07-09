#include "playback/output/iotargets/omtsink.h"

#include <QElapsedTimer>
#include <QMutexLocker>

#ifndef OLR_WITH_OMT_BUILD
std::unique_ptr<IOmtSenderBackend> makeOmtSenderBackend() {
    return std::make_unique<StubOmtSenderBackend>();
}
#endif

OmtOutputSink::OmtOutputSink() : m_ownedBackend(makeOmtSenderBackend()) {
    m_backend = m_ownedBackend.get();
}

OmtOutputSink::OmtOutputSink(IOmtSenderBackend* backend) : m_backend(backend) {}

OmtOutputSink::~OmtOutputSink() {
    stop();
}

bool OmtOutputSink::start(const OutputTargetAssignment& assignment, FrameRate rate) {
    stop();
    resetCounters();

    if (!m_backend || assignment.kind != OutputTargetKind::Omt || !assignment.enabled ||
        !rate.isValid()) {
        setStatus(OmtOutputState::InvalidAssignment,
                  QStringLiteral("invalid OMT output assignment"));
        return false;
    }

    if (!m_backend->isRuntimeAvailable()) {
        setStatus(OmtOutputState::RuntimeUnavailable,
                  QStringLiteral("OMT runtime/SDK is not available"));
        return false;
    }

    const QString senderName = senderNameFor(assignment);
    if (senderName.isEmpty()) {
        setStatus(OmtOutputState::InvalidAssignment, QStringLiteral("OMT sender name is empty"));
        return false;
    }

    if (!m_backend->createSender(senderName, rate)) {
        setStatus(OmtOutputState::CreateFailed,
                  QStringLiteral("failed to create OMT sender '%1'").arg(senderName));
        return false;
    }

    m_assignment = assignment;
    m_rate = rate;
    m_active = true;
    setStatus(OmtOutputState::Active, QStringLiteral("OMT sender '%1' active").arg(senderName));
    return true;
}

void OmtOutputSink::stop() {
    if (m_active && m_backend) m_backend->destroySender();
    m_active = false;
    setStatus(OmtOutputState::Stopped, QStringLiteral("OMT sender stopped"));
}

bool OmtOutputSink::submit(const OutputBusFrame& frame) {
    if (!m_active || !m_backend) return false;

    QElapsedTimer timer;
    timer.start();
    {
        QMutexLocker locker(&m_statusMutex);
        m_lastFrameIdentity = outputFrameIdentityFor(frame);
        m_hasLastFrameIdentity = true;
        m_lastFrameDelivered = false;
    }

    const bool ok = m_backend->sendFrame(frame);
    {
        QMutexLocker locker(&m_statusMutex);
        m_lastSubmitDurationNs = timer.nsecsElapsed();
        if (ok) {
            ++m_framesSubmitted;
            m_lastFrameDelivered = true;
            m_state = OmtOutputState::Active;
            m_message = QStringLiteral("OMT sender '%1' active").arg(senderNameFor(m_assignment));
        } else {
            ++m_sendFailures;
            m_lastFrameDelivered = false;
            m_state = OmtOutputState::SendFailed;
            m_message = QStringLiteral("failed to send OMT frame");
        }
    }
    return ok;
}

OutputSinkStatus OmtOutputSink::outputStatus() const {
    QMutexLocker locker(&m_statusMutex);
    OutputSinkStatus out;
    out.acceptedFrames = m_framesSubmitted;
    out.failedFrames = m_sendFailures;
    out.lastSubmitDurationNs = m_lastSubmitDurationNs;
    out.hasLastResult = m_framesSubmitted > 0 || m_sendFailures > 0;
    out.lastResultSucceeded = m_state != OmtOutputState::SendFailed;

    if (m_hasLastFrameIdentity) {
        out.hasLastQueuedFrameIndex = true;
        out.lastQueuedFrameIndex = m_lastFrameIdentity.outputFrameIndex;
        out.hasLastDeliveredFrameIndex = m_lastFrameDelivered && m_state == OmtOutputState::Active;
        if (out.hasLastDeliveredFrameIndex) {
            out.lastDeliveredFrameIndex = m_lastFrameIdentity.outputFrameIndex;
        }
    }

    switch (m_state) {
    case OmtOutputState::Stopped:
        out.state = QStringLiteral("stopped");
        break;
    case OmtOutputState::RuntimeUnavailable:
        out.state = QStringLiteral("runtime-unavailable");
        break;
    case OmtOutputState::InvalidAssignment:
        out.state = QStringLiteral("invalid");
        break;
    case OmtOutputState::CreateFailed:
        out.state = QStringLiteral("create-failed");
        break;
    case OmtOutputState::Active:
        out.state = QStringLiteral("active");
        break;
    case OmtOutputState::SendFailed:
        out.state = QStringLiteral("send-failed");
        break;
    }
    out.message = m_message;
    return out;
}

QString OmtOutputSink::senderNameFor(const OutputTargetAssignment& assignment) {
    QString configured =
        assignment.settings.value(QStringLiteral("senderName")).toString().trimmed();
    if (!configured.isEmpty()) return configured;
    if (!assignment.id.trimmed().isEmpty())
        return QStringLiteral("OpenLiveReplay %1").arg(assignment.id.trimmed());

    switch (assignment.sourceBus.kind) {
    case OutputBusKind::Feed:
        return QStringLiteral("OpenLiveReplay Feed %1").arg(assignment.sourceBus.index + 1);
    case OutputBusKind::Multiview:
        return QStringLiteral("OpenLiveReplay Multiview");
    case OutputBusKind::Pgm:
        return QStringLiteral("OpenLiveReplay PGM");
    }
    return QStringLiteral("OpenLiveReplay Output");
}

void OmtOutputSink::resetCounters() {
    QMutexLocker locker(&m_statusMutex);
    m_framesSubmitted = 0;
    m_sendFailures = 0;
    m_lastSubmitDurationNs = 0;
    m_hasLastFrameIdentity = false;
    m_lastFrameDelivered = false;
    m_lastFrameIdentity = OutputFrameIdentity();
}

void OmtOutputSink::setStatus(OmtOutputState state, const QString& message) {
    QMutexLocker locker(&m_statusMutex);
    m_state = state;
    m_message = message;
}
