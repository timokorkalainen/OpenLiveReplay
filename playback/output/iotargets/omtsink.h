#ifndef OMTSINK_H
#define OMTSINK_H

#include "playback/output/outputsink.h"
#include "playback/output/sinkgpucapability.h"

#include <QMutex>

#include <memory>

class IOmtSenderBackend {
public:
    virtual ~IOmtSenderBackend() = default;

    virtual bool isRuntimeAvailable() const = 0;
    virtual bool createSender(const QString& senderName, FrameRate rate) = 0;
    virtual void destroySender() = 0;
    virtual bool sendFrame(const OutputBusFrame& frame) = 0;
};

class StubOmtSenderBackend final : public IOmtSenderBackend {
public:
    bool isRuntimeAvailable() const override { return false; }
    bool createSender(const QString&, FrameRate) override { return false; }
    void destroySender() override {}
    bool sendFrame(const OutputBusFrame&) override { return false; }
};

enum class OmtOutputState {
    Stopped,
    RuntimeUnavailable,
    InvalidAssignment,
    CreateFailed,
    Active,
    SendFailed,
};

class OmtOutputSink final : public IOutputSink {
public:
    OmtOutputSink();
    explicit OmtOutputSink(IOmtSenderBackend* backend);
    ~OmtOutputSink() override;

    OutputTargetKind kind() const override { return OutputTargetKind::Omt; }
    bool start(const OutputTargetAssignment& assignment, FrameRate rate) override;
    void stop() override;
    bool isActive() const override { return m_active; }
    bool submit(const OutputBusFrame& frame) override;
    OutputSinkStatus outputStatus() const override;
    bool needsContinuousCadence() const override { return true; }
    SinkGpuCapability resolvedCapability() const {
        return SinkGpuCapability::NeedsContinuousCadence;
    }

private:
    static QString senderNameFor(const OutputTargetAssignment& assignment);
    void resetCounters();
    void setStatus(OmtOutputState state, const QString& message);

    std::unique_ptr<IOmtSenderBackend> m_ownedBackend;
    IOmtSenderBackend* m_backend = nullptr;
    OutputTargetAssignment m_assignment;
    FrameRate m_rate;
    bool m_active = false;
    mutable QMutex m_statusMutex;
    OmtOutputState m_state = OmtOutputState::Stopped;
    QString m_message;
    qint64 m_framesSubmitted = 0;
    qint64 m_sendFailures = 0;
    qint64 m_lastSubmitDurationNs = 0;
    bool m_hasLastFrameIdentity = false;
    bool m_lastFrameDelivered = false;
    OutputFrameIdentity m_lastFrameIdentity;
};

std::unique_ptr<IOmtSenderBackend> makeOmtSenderBackend();

#endif // OMTSINK_H
