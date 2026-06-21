#include "playback/output/qtpreviewsink.h"

#include "playback/frameprovider.h"

#include <QSize>
#include <QVideoFrameFormat>
#include <QtGlobal>
#include <cstring>

QtPreviewSink::QtPreviewSink(FrameProvider* provider) : m_provider(provider) {}

bool QtPreviewSink::deliver(const FrameHandle& frame) {
    if (!m_provider) return false;
    QVideoFrame qFrame = toQVideoFrame(frame);
    if (!qFrame.isValid()) return false;
    m_provider->deliverFrame(qFrame);
    return true;
}

QVideoFrameFormat::ColorSpace qtColorSpaceFor(ColorMatrix matrix) {
    switch (matrix) {
    case ColorMatrix::Bt601:
        return QVideoFrameFormat::ColorSpace_BT601;
    case ColorMatrix::Bt2020:
        return QVideoFrameFormat::ColorSpace_BT2020;
    case ColorMatrix::Bt709:
        return QVideoFrameFormat::ColorSpace_BT709;
    }
    return QVideoFrameFormat::ColorSpace_BT709;
}

QVideoFrameFormat::ColorRange qtColorRangeFor(ColorRange range) {
    return range == ColorRange::Full ? QVideoFrameFormat::ColorRange_Full
                                     : QVideoFrameFormat::ColorRange_Video;
}

QVideoFrame QtPreviewSink::toQVideoFrame(const FrameHandle& frame) {
    const MediaVideoFrameView view(frame);
    if (!view.isValid()) return QVideoFrame();
    QVideoFrameFormat format(QSize(view.width, view.height), QVideoFrameFormat::Format_YUV420P);
    const ColorMetadata& color = frame.metadata().color;
    format.setColorSpace(qtColorSpaceFor(color.matrix));
    format.setColorRange(qtColorRangeFor(color.range));

    QVideoFrame qFrame(format);
    if (!qFrame.map(QVideoFrame::WriteOnly)) return QVideoFrame();

    const QByteArray planes[3] = {view.planeY, view.planeU, view.planeV};
    const int srcStrides[3] = {view.strideY, view.strideU, view.strideV};
    for (int i = 0; i < 3; ++i) {
        const int height = (i == 0) ? view.height : (view.height + 1) / 2;
        const int width = (i == 0) ? view.width : (view.width + 1) / 2;
        const int copyW = qMin(width, qMin(srcStrides[i], qFrame.bytesPerLine(i)));
        for (int y = 0; y < height; ++y) {
            std::memcpy(qFrame.bits(i) + static_cast<qsizetype>(y) * qFrame.bytesPerLine(i),
                        planes[i].constData() + static_cast<qsizetype>(y) * srcStrides[i],
                        size_t(copyW));
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
