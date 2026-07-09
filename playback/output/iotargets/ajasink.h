#ifndef AJASINK_H
#define AJASINK_H

#include "playback/output/outputsink.h"
#include "playback/output/sinkgpucapability.h"

#include <QMutex>

#include <memory>

class IAjaSenderBackend {
public:
    virtual ~IAjaSenderBackend() = default;

    virtual bool isRuntimeAvailable() const = 0;
    virtual bool openDevice(const OutputTargetAssignment& assignment, FrameRate rate) = 0;
    virtual void closeDevice() = 0;
    virtual bool transferFrame(const OutputBusFrame& frame) = 0;
};

class StubAjaSenderBackend final : public IAjaSenderBackend {
public:
    bool isRuntimeAvailable() const override { return false; }
    bool openDevice(const OutputTargetAssignment&, FrameRate) override { return false; }
    void closeDevice() override {}
    bool transferFrame(const OutputBusFrame&) override { return false; }
};

enum class AjaOutputState {
    Stopped,
    RuntimeUnavailable,
    InvalidAssignment,
    OpenFailed,
    Active,
    SendFailed,
};

class AjaOutputSink final : public IOutputSink {
public:
    AjaOutputSink();
    explicit AjaOutputSink(IAjaSenderBackend* backend);
    ~AjaOutputSink() override;

    OutputTargetKind kind() const override { return OutputTargetKind::Aja; }
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
    void resetCounters();
    void setStatus(AjaOutputState state, const QString& message);

    std::unique_ptr<IAjaSenderBackend> m_ownedBackend;
    IAjaSenderBackend* m_backend = nullptr;
    OutputTargetAssignment m_assignment;
    FrameRate m_rate;
    bool m_active = false;
    mutable QMutex m_statusMutex;
    AjaOutputState m_state = AjaOutputState::Stopped;
    QString m_message;
    qint64 m_framesSubmitted = 0;
    qint64 m_sendFailures = 0;
    qint64 m_lastSubmitDurationNs = 0;
    bool m_hasLastFrameIdentity = false;
    bool m_lastFrameDelivered = false;
    OutputFrameIdentity m_lastFrameIdentity;
};

std::unique_ptr<IAjaSenderBackend> makeAjaSenderBackend();

#endif // AJASINK_H
