#ifndef QTPREVIEWSINK_H
#define QTPREVIEWSINK_H

#include "playback/output/mediaframe.h"
#include "playback/output/outputsink.h"

#include <QVideoFrame>

class FrameProvider;

class QtPreviewSink {
public:
    explicit QtPreviewSink(FrameProvider* provider);

    bool deliver(const MediaVideoFrame& frame);
    static QVideoFrame toQVideoFrame(const MediaVideoFrame& frame);

private:
    FrameProvider* m_provider = nullptr;
};

class QtPreviewOutputSink final : public IOutputSink {
public:
    explicit QtPreviewOutputSink(FrameProvider* provider);

    OutputTargetKind kind() const override { return OutputTargetKind::QtPreview; }
    bool start(const OutputTargetAssignment& assignment, FrameRate rate) override;
    void stop() override;
    bool isActive() const override { return m_active; }
    bool submit(const OutputBusFrame& frame) override;

private:
    FrameProvider* m_provider = nullptr;
    OutputTargetAssignment m_assignment;
    bool m_active = false;
};

#endif // QTPREVIEWSINK_H
