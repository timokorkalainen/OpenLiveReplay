#ifndef QTPREVIEWSINK_H
#define QTPREVIEWSINK_H

#include "playback/output/framehandle.h"
#include "playback/output/outputsink.h"

#include <QVideoFrame>
#include <QVideoFrameFormat>

class FrameProvider;

QVideoFrameFormat::ColorSpace qtColorSpaceFor(ColorMatrix matrix);
QVideoFrameFormat::ColorRange qtColorRangeFor(ColorRange range);

class QtPreviewSink {
public:
    explicit QtPreviewSink(FrameProvider* provider);

    bool deliver(const FrameHandle& frame, quint64* deliveredSerial = nullptr);
    static QVideoFrame toQVideoFrame(const FrameHandle& frame);

private:
    FrameProvider* m_provider = nullptr;
};

class QtPreviewOutputSink final : public IOutputSink {
public:
    explicit QtPreviewOutputSink(FrameProvider* provider);

    OutputTargetKind kind() const override { return OutputTargetKind::QtPreview; }
    bool start(const OutputTargetAssignment& assignment, FrameRate rate) override;
    void stop() override;
    bool isActive() const override;
    bool submit(const OutputBusFrame& frame) override;
    bool flush(int timeoutMs) override;

private:
    FrameProvider* m_provider = nullptr;
    OutputTargetAssignment m_assignment;
    quint64 m_lastSubmittedSerial = 0;
    bool m_active = false;
};

#endif // QTPREVIEWSINK_H
