#ifndef NDISINK_H
#define NDISINK_H

#include "playback/output/outputsink.h"

#include <memory>

class INdiSenderBackend {
public:
    virtual ~INdiSenderBackend() = default;

    virtual bool isRuntimeAvailable() const = 0;
    virtual bool createSender(const QString& senderName, FrameRate rate) = 0;
    virtual void destroySender() = 0;
    virtual bool sendFrame(const OutputBusFrame& frame) = 0;
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

private:
    static QString senderNameFor(const OutputTargetAssignment& assignment);

    std::unique_ptr<INdiSenderBackend> m_ownedBackend;
    INdiSenderBackend* m_backend = nullptr;
    OutputTargetAssignment m_assignment;
    FrameRate m_rate;
    bool m_active = false;
};

#endif // NDISINK_H
