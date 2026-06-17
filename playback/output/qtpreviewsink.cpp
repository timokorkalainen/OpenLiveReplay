#include "playback/output/qtpreviewsink.h"

#include "playback/frameprovider.h"

#include <QSize>
#include <QVideoFrameFormat>
#include <QtGlobal>
#include <cstring>

QtPreviewSink::QtPreviewSink(FrameProvider* provider) : m_provider(provider) {}

bool QtPreviewSink::deliver(const MediaVideoFrame& frame) {
    if (!m_provider) return false;
    QVideoFrame qFrame = toQVideoFrame(frame);
    if (!qFrame.isValid()) return false;
    m_provider->deliverFrame(qFrame);
    return true;
}

QVideoFrame QtPreviewSink::toQVideoFrame(const MediaVideoFrame& frame) {
    if (!frame.isValid()) return QVideoFrame();
    QVideoFrameFormat format(QSize(frame.width, frame.height), QVideoFrameFormat::Format_YUV420P);
    format.setColorSpace(frame.height > 576 ? QVideoFrameFormat::ColorSpace_BT709
                                            : QVideoFrameFormat::ColorSpace_BT601);
    format.setColorRange(QVideoFrameFormat::ColorRange_Video);

    QVideoFrame qFrame(format);
    if (!qFrame.map(QVideoFrame::WriteOnly)) return QVideoFrame();

    const QByteArray planes[3] = {frame.planeY, frame.planeU, frame.planeV};
    const int srcStrides[3] = {frame.strideY, frame.strideU, frame.strideV};
    for (int i = 0; i < 3; ++i) {
        const int height = (i == 0) ? frame.height : (frame.height + 1) / 2;
        const int width = (i == 0) ? frame.width : (frame.width + 1) / 2;
        const int copyW = qMin(width, qMin(srcStrides[i], qFrame.bytesPerLine(i)));
        for (int y = 0; y < height; ++y) {
            std::memcpy(qFrame.bits(i) + y * qFrame.bytesPerLine(i),
                        planes[i].constData() + y * srcStrides[i], size_t(copyW));
        }
    }
    qFrame.unmap();
    return qFrame;
}

QtPreviewOutputSink::QtPreviewOutputSink(FrameProvider* provider) : m_provider(provider) {}

bool QtPreviewOutputSink::start(const OutputTargetAssignment& assignment, FrameRate rate) {
    if (!m_provider || assignment.kind != OutputTargetKind::QtPreview || !assignment.enabled ||
        !rate.isValid()) {
        m_active = false;
        return false;
    }
    m_assignment = assignment;
    m_active = true;
    return true;
}

void QtPreviewOutputSink::stop() {
    m_active = false;
}

bool QtPreviewOutputSink::submit(const OutputBusFrame& frame) {
    if (!m_active) return false;
    QtPreviewSink sink(m_provider);
    return sink.deliver(frame.video);
}
