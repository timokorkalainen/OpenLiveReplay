#ifndef DECKLINKSINK_H
#define DECKLINKSINK_H

#include "playback/output/outputsink.h"
#include "playback/output/sinkgpucapability.h"
#include "playback/output/iotargets/st2110framer.h"

#include <QMutex>

#include <memory>
#include <utility>

class IDeckLinkSenderBackend {
public:
    virtual ~IDeckLinkSenderBackend() = default;

    virtual bool isRuntimeAvailable() const = 0;
    virtual bool deviceSupportsGpuTextureInput() const = 0;
    virtual bool openDevice(const OutputTargetAssignment& assignment, FrameRate rate) = 0;
    virtual void closeDevice() = 0;
    virtual bool scheduleFrame(OutputBusFrame frame) = 0;
    virtual bool scheduleGpuFrame(OutputBusFrame frame) = 0;
    virtual bool scheduleSt2110Frame(OutputBusFrame frame, St2110VideoFrame st2110Frame) {
        Q_UNUSED(st2110Frame);
        return scheduleFrame(std::move(frame));
    }
    virtual bool scheduleGpuSt2110Frame(OutputBusFrame frame, St2110VideoFrame st2110Frame) {
        Q_UNUSED(st2110Frame);
        return scheduleGpuFrame(std::move(frame));
    }
};

class StubDeckLinkSenderBackend final : public IDeckLinkSenderBackend {
public:
    bool isRuntimeAvailable() const override { return false; }
    bool deviceSupportsGpuTextureInput() const override { return false; }
    bool openDevice(const OutputTargetAssignment&, FrameRate) override { return false; }
    void closeDevice() override {}
    bool scheduleFrame(OutputBusFrame) override { return false; }
    bool scheduleGpuFrame(OutputBusFrame) override { return false; }
};

enum class DeckLinkOutputState {
    Stopped,
    RuntimeUnavailable,
    InvalidAssignment,
    OpenFailed,
    Active,
    SendFailed,
};

class DeckLinkOutputSink final : public IOutputSink {
public:
    explicit DeckLinkOutputSink(OutputTargetKind kind);
    DeckLinkOutputSink(OutputTargetKind kind, IDeckLinkSenderBackend* backend);
    ~DeckLinkOutputSink() override;

    OutputTargetKind kind() const override { return m_kind; }
    bool start(const OutputTargetAssignment& assignment, FrameRate rate) override;
    void stop() override;
    bool isActive() const override { return m_active; }
    bool submit(const OutputBusFrame& frame) override;
    OutputSinkStatus outputStatus() const override;
    bool needsContinuousCadence() const override { return true; }
    SinkGpuCapability resolvedCapability() const;
    SinkGpuCapability capabilityForAssignment(const OutputTargetAssignment& assignment) const;

private:
    static bool isDeckLinkKind(OutputTargetKind kind);
    void resetCounters();
    void setStatus(DeckLinkOutputState state, const QString& message);

    OutputTargetKind m_kind;
    std::unique_ptr<IDeckLinkSenderBackend> m_ownedBackend;
    IDeckLinkSenderBackend* m_backend = nullptr;
    OutputTargetAssignment m_assignment;
    FrameRate m_rate;
    bool m_active = false;
    SinkGpuCapability m_capability = SinkGpuCapability::NeedsContinuousCadence;
    mutable QMutex m_statusMutex;
    DeckLinkOutputState m_state = DeckLinkOutputState::Stopped;
    QString m_message;
    qint64 m_framesSubmitted = 0;
    qint64 m_sendFailures = 0;
    qint64 m_lastSubmitDurationNs = 0;
    bool m_hasLastFrameIdentity = false;
    bool m_lastFrameDelivered = false;
    OutputFrameIdentity m_lastFrameIdentity;
};

std::unique_ptr<IDeckLinkSenderBackend> makeDeckLinkSenderBackend();

#endif // DECKLINKSINK_H
