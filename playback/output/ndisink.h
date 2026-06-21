#ifndef NDISINK_H
#define NDISINK_H

#include "playback/output/ndiabi.h"
#include "playback/output/outputsink.h"

#include <QMutex>
#include <memory>

// Resolves the NDI frame `timecode` field (100 ns) from a bus frame's programme timecode:
// a value >= 0 passes through; -1 (unset) maps to the NDI "synthesize" sentinel so the SDK
// generates one. Exposed for unit testing the mapping.
qint64 resolveNdiTimecode(qint64 programmeTimecode100ns);

// Stamps the bus frame's programme timecode onto the paired NDI video + audio frames of one
// tick (both carry the SAME timecode so the A/V pair stays aligned). The NDI `timestamp` is
// left to the SDK, which overwrites it on send with its own submission time. Exposed so the
// sink's bus-frame -> NDI-struct timecode mapping is unit-testable without a live NDI runtime.
void applyNdiFrameTiming(const OutputBusFrame& frame, olr::ndi::NDIlib_video_frame_v2_t& video,
                         olr::ndi::NDIlib_audio_frame_v3_t& audio);

class INdiSenderBackend {
public:
    virtual ~INdiSenderBackend() = default;

    virtual bool isRuntimeAvailable() const = 0;
    virtual bool createSender(const QString& senderName, FrameRate rate) = 0;
    virtual void destroySender() = 0;
    virtual bool sendFrame(const OutputBusFrame& frame) = 0;
};

enum class NdiOutputState {
    Stopped,
    RuntimeUnavailable,
    InvalidAssignment,
    CreateFailed,
    Active,
    SendFailed,
};

struct NdiOutputStatus {
    NdiOutputState state = NdiOutputState::Stopped;
    QString message;
    qint64 framesSubmitted = 0;
    qint64 sendFailures = 0;
    qint64 lastSendDurationNs = 0;
    bool hasLastFrameIdentity = false;
    bool lastFrameDelivered = false;
    OutputFrameIdentity lastFrameIdentity;
};

class NdiOutputSink final : public IOutputSink {
public:
    NdiOutputSink();
    explicit NdiOutputSink(INdiSenderBackend* backend);
    ~NdiOutputSink() override;

    OutputTargetKind kind() const override { return OutputTargetKind::Ndi; }
    bool start(const OutputTargetAssignment& assignment, FrameRate rate) override;
    void stop() override;
    bool isActive() const override { return m_active; }
    bool submit(const OutputBusFrame& frame) override;
    NdiOutputStatus status() const;
    OutputSinkStatus outputStatus() const override;

private:
    static QString senderNameFor(const OutputTargetAssignment& assignment);
    void setStatus(NdiOutputState state, const QString& message);

    std::unique_ptr<INdiSenderBackend> m_ownedBackend;
    INdiSenderBackend* m_backend = nullptr;
    OutputTargetAssignment m_assignment;
    FrameRate m_rate;
    bool m_active = false;
    mutable QMutex m_statusMutex;
    NdiOutputStatus m_status;
};

#endif // NDISINK_H
